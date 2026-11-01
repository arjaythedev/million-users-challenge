package feed.tune

import kotlin.concurrent.atomics.AtomicInt
import kotlin.jvm.JvmField
import kotlin.math.abs

/**
 * Event-loop wake-up batching. After a wake-up that carried fewer than [MIN_EVENTS] pieces of work a loop sleeps
 * [sleepUs] before it polls again, so one wake-up serves everything that arrived meanwhile: fewer epoll_wait calls,
 * context switches and, in a VM, wake-up interrupts. 0 = off. The other top entries use a fixed 2 ms; what it is worth
 * depends on the box (on the lab laptop it saves nothing, in a VM it can save a lot), so [BatchTuner] looks for a better
 * setting while the server runs, from measurements, and otherwise keeps that 2 ms.
 */
object LoopTuning {
    val sleepUs = AtomicInt(0)
    const val MIN_EVENTS = 32
}

/** What the stats thread measured over one epoch (a few seconds). */
class Epoch(
    @JvmField val secs: Double,
    @JvmField val requests: Long,
    /** CPU of the CPUs this process may run on (user, system, irq, softirq, steal) per request, in microseconds; <= 0 if unknown. */
    @JvmField val costUs: Double,
    @JvmField val p99Us: Long,
    /** Requests refused because a queue was full during the epoch. */
    @JvmField val shed: Long,
    /** Busy share of those CPUs over the epoch, 0..1; negative if unknown. */
    @JvmField val busy: Double = -1.0,
)

/**
 * Chooses [LoopTuning.sleepUs] by online A/B tests, and is built never to be the reason a run fails:
 *  - The first of [settingsUs] is the prior (the other top entries' fixed 2 ms) and is what runs until a probe clearly beats
 *    it. A different setting must be [adoptMargin] cheaper in CPU per request to replace the prior (the prior wins ties:
 *    returning to it needs only [returnMargin]), and a decision made at one load is dropped, back to the prior, when the
 *    load later moves by more than [movedFraction], because what batching is worth grows with the load.
 *  - It only explores while the server is comfortable: an epoch with enough requests, a p99 under [comfortP99Us], nothing
 *    shed and the CPUs under [comfortBusy]. Near the knee it holds what it has.
 *  - A probe is an ABBA-style schedule of eight epochs (A B B A B A A B: equal mean positions, so a drifting load cancels)
 *    of the held setting (A) and one alternative (B). It is abandoned at once, back to the held setting, when any epoch of it is
 *    not comfortable or when an epoch of the alternative has more than twice the p99 of the held setting.
 *  - A probe needs at least [minRps] requests per second and is dropped when the load differs by more than 10% between its arms.
 * A new probe starts after [holdEpochs] epochs (after [RETRY_EPOCHS] when the last one could not decide, after [ABORT_EPOCHS]
 * when it was abandoned), or sooner when the load moved.
 */
class BatchTuner(
    private val settingsUs: IntArray,
    private val minRps: Double = 1500.0,
    private val adoptMargin: Double = 0.08,
    private val returnMargin: Double = 0.01,
    private val comfortP99Us: Long = 100_000,
    private val comfortBusy: Double = 0.7,
    private val minSamples: Long = 1000,
    private val movedFraction: Double = 0.35,
    private val holdEpochs: Int = 24,
    private val log: (String) -> Unit = {},
) {
    init {
        require(settingsUs.isNotEmpty() && settingsUs.all { it >= 0 })
    }

    /** The prior: what the server starts with and returns to. */
    val priorUs: Int get() = settingsUs[0]

    /** The setting for the next epoch, in microseconds (0 = off). */
    var current: Int = settingsUs[0]
        private set

    /** The setting held between probes. */
    val chosen: Int get() = best

    /** Probes finished so far. */
    var probes = 0
        private set

    private var best = settingsUs[0]
    private var altIndex = 0
    private var altSetting = 0
    private var probing = false
    private var step = 0
    private var invalid = 0
    private val costSum = DoubleArray(2) // by arm: 0 = the held setting, 1 = the alternative
    private val reqs = DoubleArray(2)
    private val secs = DoubleArray(2)
    private var holdLeft = 0
    private var steady = 0
    private var lastRps = 0.0
    private var decisionRps = 0.0
    private var bestP99Us = 0L // p99 of the latest epoch that ran the held setting with enough requests

    /** Takes the measurements of the epoch that just ended; returns the setting to run during the next one. */
    fun onEpoch(e: Epoch): Int {
        val rps = if (e.secs > 0) e.requests / e.secs else 0.0
        val enough = e.requests >= minSamples
        val comfortable = enough && e.p99Us < comfortP99Us && e.shed == 0L && (e.busy < 0 || e.busy < comfortBusy)
        val valid = e.costUs > 0 && rps >= minRps
        if (probing) probeEpoch(e, valid, enough, comfortable) else holdEpoch(e, rps, valid, enough, comfortable)
        lastRps = rps
        return current
    }

    fun status(): String = "tune[loop_sleep=${current}us held=${best}us probes=$probes]"

    private fun holdEpoch(e: Epoch, rps: Double, valid: Boolean, enough: Boolean, comfortable: Boolean) {
        if (enough && current == best) bestP99Us = e.p99Us
        steady = if (valid && comfortable && lastRps > 0 && abs(rps / lastRps - 1) < 0.25) steady + 1 else 0
        if (holdLeft > 0) holdLeft--
        val moved = decisionRps > 0 && abs(rps / decisionRps - 1) > movedFraction
        if (moved && best != priorUs) {
            log("tune: the load moved from ${decisionRps.toLong()} to ${rps.toLong()} req/s: back to the prior $priorUs us")
            best = priorUs
            current = priorUs
            decisionRps = 0.0
            holdLeft = 0
            steady = 0
            return
        }
        if (steady >= 2 && (holdLeft == 0 || (moved && holdLeft <= holdEpochs - 6))) startProbe()
    }

    private fun startProbe() {
        if (best != priorUs && probes % 2 == 0) {
            altSetting = priorUs // coming back to the prior is checked first
        } else {
            var tries = 0
            do {
                altIndex = (altIndex + 1) % settingsUs.size
                tries++
            } while (settingsUs[altIndex] == best && tries <= settingsUs.size)
            altSetting = settingsUs[altIndex]
        }
        if (altSetting == best) return // a single setting: nothing to compare
        probing = true
        step = 0
        invalid = 0
        costSum.fill(0.0)
        reqs.fill(0.0)
        secs.fill(0.0)
        current = armSetting(SCHEDULE[0]) // the next epoch is the first of the schedule: the held setting, as now
    }

    private fun armSetting(alt: Boolean) = if (alt) altSetting else best

    private fun probeEpoch(e: Epoch, valid: Boolean, enough: Boolean, comfortable: Boolean) {
        val altArm = SCHEDULE[step]
        val arm = if (altArm) 1 else 0
        if (enough && !altArm) bestP99Us = e.p99Us
        if ((enough && !comfortable) || e.shed > 0) {
            log("tune: probe of $altSetting us against $best us abandoned: the server is no longer comfortable (p99 ${e.p99Us / 1000} ms, shed ${e.shed})")
            endProbe(ABORT_EPOCHS)
            return
        }
        if (altArm && enough && e.p99Us > 20_000 && e.p99Us > 2 * bestP99Us) {
            log("tune: probe of $altSetting us against $best us abandoned: p99 ${e.p99Us / 1000} ms against ${bestP99Us / 1000} ms")
            endProbe(ABORT_EPOCHS)
            return
        }
        if (valid) {
            costSum[arm] += e.costUs * e.requests
            reqs[arm] += e.requests.toDouble()
            secs[arm] += e.secs
        } else if (++invalid > 3) {
            log("tune: probe of $altSetting us against $best us dropped: load below $minRps req/s or unsteady")
            endProbe(RETRY_EPOCHS)
            return
        }
        step++
        if (step < SCHEDULE.size) {
            current = armSetting(SCHEDULE[step])
            return
        }
        decide()
    }

    private fun decide() {
        if (reqs[0] <= 0.0 || reqs[1] <= 0.0) {
            endProbe(RETRY_EPOCHS)
            return
        }
        val c0 = costSum[0] / reqs[0]
        val c1 = costSum[1] / reqs[1]
        val r0 = reqs[0] / secs[0]
        val r1 = reqs[1] / secs[1]
        val comparable = abs(r1 / r0 - 1) < 0.1
        val needed = if (altSetting == priorUs) returnMargin else adoptMargin
        val before = best
        val verdict = when {
            !comparable -> "load moved between the arms, no decision"
            c1 < c0 * (1 - needed) -> {
                best = altSetting
                "switch to $best us"
            }
            else -> "keep $best us"
        }
        decisionRps = (r0 + r1) / 2
        log("tune: $before us ${one(c0)} us/req vs $altSetting us ${one(c1)} us/req at ${r0.toLong()} req/s: $verdict")
        probes++
        endProbe(if (comparable) holdEpochs else RETRY_EPOCHS)
    }

    private fun endProbe(hold: Int) {
        probing = false
        current = best
        holdLeft = hold
        steady = 0
    }

    private fun one(v: Double): String {
        val x = kotlin.math.round(v * 10).toLong()
        return "${x / 10}.${abs(x % 10)}"
    }

    private companion object {
        /** true = the alternative runs that epoch: A B B A B A A B (A and B each at positions summing to 14). */
        val SCHEDULE = booleanArrayOf(false, true, true, false, true, false, false, true)
        /** After a probe that could not decide (the load moved, too few valid epochs) the next one starts soon. */
        const val RETRY_EPOCHS = 4
        /** After a probe was abandoned (the server stopped being comfortable, or the alternative hurt) it waits a minute. */
        const val ABORT_EPOCHS = 12
    }
}
