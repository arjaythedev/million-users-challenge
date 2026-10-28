import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
    kotlin("multiplatform") version "2.4.21"
}

kotlin {
    // The same tree also builds for the JVM: a free A/B against the native binary (see README).
    jvm {
        compilerOptions { jvmTarget.set(JvmTarget.JVM_25) }
    }
    linuxX64 {
        binaries {
            executable {
                entryPoint = "feed.main"
                baseName = "feed"
                // LLVM codegen for the build machine's CPU (= the box: build.sh runs there). build.sh detects the
                // CPU name and its full +/- feature list with Kotlin/Native's own clang (-march=native); the full
                // list matters, a bare CPU name could emit instructions a hypervisor masks (SIGILL). Without the
                // properties: Kotlin/Native's portable default (x86-64, SSE2).
                val cpu = providers.gradleProperty("feed.targetCpu").orNull?.trim()
                val features = providers.gradleProperty("feed.targetCpuFeatures").orNull?.trim()
                if (!cpu.isNullOrEmpty() && !features.isNullOrEmpty()) {
                    freeCompilerArgs += "-Xoverride-konan-properties=" +
                        "targetCpu.linux_x64=$cpu;targetCpuFeatures.linux_x64=$features"
                }
                freeCompilerArgs += "-Xbinary=preCodegenInlineThreshold=40"
            }
        }
    }

    compilerOptions {
        optIn.addAll(
            "kotlin.concurrent.atomics.ExperimentalAtomicApi",
            "kotlin.experimental.ExperimentalNativeApi",
            "kotlinx.cinterop.ExperimentalForeignApi",
        )
        freeCompilerArgs.add("-Xexpect-actual-classes")
    }

    sourceSets {
        commonTest.dependencies {
            implementation(kotlin("test"))
        }
    }
}

// build/jvm/feed.jar + build/jvm/lib/*.jar (kotlin-stdlib): the JVM variant, for A/B runs only
val installJvm by tasks.registering(Copy::class) {
    val jar = tasks.named("jvmJar")
    dependsOn(jar)
    from(jar)
    from(configurations.named("jvmRuntimeClasspath")) { into("lib") }
    into(layout.buildDirectory.dir("jvm"))
    rename("feed-jvm.jar", "feed.jar")
}
