package feed.tune

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertTrue

class TunerTest {
    private val settings = intArrayOf(2000, 0, 1000, 4000, 6000, 8000)

    /** Runs [n] epochs of 5 s; cost, p99, shed and busy are functions of the setting the tuner chose for that epoch. */
    private fun run(
        t: BatchTuner, n: Int, rps: (Int) -> Double = { 20_000.0 }, p99Us: (Int, Int) -> Long = { _, _ -> 20_000 },
        shed: (Int, Int) -> Long = { _, _ -> 0 }, busy: (Int, Int) -> Double = { _, _ -> 0.3 }, cost: (Int) -> Double,
    ): List<Int> {
        val used = ArrayList<Int>()
        for (i in 0 until n) {
            val s = t.current
            used.add(s)
            t.onEpoch(Epoch(5.0, (rps(i) * 5).toLong(), cost(s), p99Us(i, s), shed(i, s), busy(i, s)))
        }
        return used
    }

    @Test
    fun keepsThePriorWhenItIsTheCheapest() {
        val log = ArrayList<String>()
        val t = BatchTuner(settings, log = { log.add(it) })
        val used = run(t, 400) { when (it) { 2000 -> 40.0; 0 -> 50.0; 1000 -> 45.0; 4000 -> 42.0; 6000 -> 44.0; else -> 43.0 } }
        assertEquals(2000, t.chosen)
        assertTrue(t.probes >= 6, "probes ${t.probes}")
        assertTrue(log.all { it.contains("keep 2000") }, log.toString())
        assertTrue(used.count { it != 2000 } <= used.size / 2, "the probes' alternative arms are at most half of a probe")
    }

    @Test
    fun anAlternativeNeedsEightPercentToReplaceThePrior() {
        val t = BatchTuner(settings)
        run(t, 300) { if (it == 0) 50.0 else 52.0 } // off is 3.8% cheaper: not enough
        assertEquals(2000, t.chosen)
        assertTrue(t.probes >= 5)
        val u = BatchTuner(settings)
        run(u, 60) { if (it == 0) 47.5 else 52.0 } // off is 8.7% cheaper: enough
        assertEquals(0, u.chosen)
    }

    @Test
    fun thePriorWinsClosePlacesBackOverTheAlternative() {
        val t = BatchTuner(settings)
        var phase2 = false
        val c = { s: Int -> if (!phase2) (if (s == 0) 40.0 else 50.0) else (if (s == 2000) 39.0 else 40.0) }
        run(t, 60, cost = c)
        assertEquals(0, t.chosen)
        phase2 = true
        run(t, 300, cost = c) // the prior is 2.5% cheaper than the held off: returning needs only 1%
        assertEquals(2000, t.chosen)
    }

    @Test
    fun aDecisionDoesNotCarryOverToAnotherLoad() {
        val log = ArrayList<String>()
        val t = BatchTuner(settings, log = { log.add(it) })
        val c = { s: Int -> if (s == 0) 40.0 else 50.0 }
        run(t, 60, cost = c)
        assertEquals(0, t.chosen) // off is 20% cheaper at 20k req/s
        run(t, 2, rps = { 40_000.0 }, cost = c) // the load doubles
        assertEquals(2000, t.chosen)
        assertTrue(log.any { it.contains("the load moved") }, log.toString())
    }

    @Test
    fun anAlternativeThatHurtsTheTailIsDroppedAfterOneEpoch() {
        val log = ArrayList<String>()
        val t = BatchTuner(settings, log = { log.add(it) })
        // off makes the p99 four times worse (still under the 100 ms comfort line): abandon at once, never decide
        val used = run(t, 14, p99Us = { _, s -> if (s == 0) 80_000 else 20_000 }) { 40.0 }
        assertTrue(log.first().contains("abandoned"), log.toString())
        assertEquals(1, used.count { it == 0 }, "the hurting arm ran for exactly one epoch: $used")
        assertEquals(2000, t.chosen)
        assertEquals(0, t.probes)
        // and the next probe waits about a minute (12 epochs)
        assertTrue(used.takeLast(6).all { it == 2000 })
    }

    @Test
    fun noProbeWhileTheServerIsNotComfortable() {
        for (case in 0 until 4) {
            val t = BatchTuner(settings)
            val used = run(
                t, 200,
                p99Us = { _, _ -> if (case == 0) 150_000 else 20_000 },
                shed = { _, _ -> if (case == 1) 3 else 0 },
                busy = { _, _ -> if (case == 2) 0.85 else 0.3 },
                rps = { if (case == 3) 100.0 else 20_000.0 }, // 500 requests per epoch: too few to trust a p99
            ) { if (it == 2000) 50.0 else 30.0 }
            assertTrue(used.all { it == 2000 }, "case $case: $used")
            assertEquals(0, t.probes, "case $case")
        }
    }

    @Test
    fun aProbeEndsAtOnceWhenTheServerGetsStressed() {
        val log = ArrayList<String>()
        val t = BatchTuner(settings, log = { log.add(it) })
        val used = run(t, 60, p99Us = { i, _ -> if (i >= 7) 150_000 else 20_000 }) { 40.0 }
        assertTrue(log.any { it.contains("no longer comfortable") }, log.toString())
        assertTrue(used.drop(8).all { it == 2000 }, "$used")
        assertEquals(2000, t.chosen)
    }

    @Test
    fun noProbeBelowTheMinimumLoad() {
        val t = BatchTuner(settings)
        val used = run(t, 200, rps = { 500.0 }) { if (it == 2000) 90.0 else 10.0 }
        assertTrue(used.all { it == 2000 })
        assertEquals(0, t.probes)
    }

    @Test
    fun noProbeWhileTheLoadJumps() {
        val t = BatchTuner(settings)
        val used = run(t, 200, rps = { i -> if (i % 2 == 0) 10_000.0 else 30_000.0 }) { if (it == 2000) 90.0 else 10.0 }
        assertTrue(used.all { it == 2000 })
        assertEquals(0, t.probes)
    }

    @Test
    fun aLinearRampDoesNotFoolTheComparison() {
        val t = BatchTuner(settings)
        // the same cost for every setting, the load rising 0.3% per epoch: ABBA must call it a tie and keep the prior
        run(t, 300, rps = { i -> 12_000.0 * (1 + 0.003 * i) }) { 40.0 }
        assertEquals(2000, t.chosen)
        assertTrue(t.probes >= 3, "probes ${t.probes}")
    }

    @Test
    fun findsAClearlyCheaperLongerWindow() {
        val t = BatchTuner(settings)
        run(t, 500) { when (it) { 4000 -> 35.0; 2000 -> 40.0; 1000 -> 44.0; 6000 -> 37.0; 8000 -> 38.0; else -> 50.0 } }
        assertEquals(4000, t.chosen)
    }

    @Test
    fun aSingleSettingNeverProbes() {
        val t = BatchTuner(intArrayOf(1500))
        val used = run(t, 100) { 40.0 }
        assertTrue(used.all { it == 1500 })
        assertEquals(0, t.probes)
    }

    @Test
    fun costIsUnknownWithoutProcStat() {
        val t = BatchTuner(settings)
        val used = run(t, 100) { 0.0 } // no measurement: stay where we are
        assertTrue(used.all { it == 2000 })
    }

    @Test
    fun theEpochConstructorDefaultsBusyToUnknown() {
        val t = BatchTuner(settings)
        // busy unknown (-1) must not block probing
        for (i in 0 until 100) t.onEpoch(Epoch(5.0, 100_000, 40.0, 20_000, 0))
        assertTrue(t.probes >= 1)
    }
}
