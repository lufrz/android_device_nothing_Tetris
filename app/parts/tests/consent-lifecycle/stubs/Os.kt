// SPDX-License-Identifier: Apache-2.0
package android.os

object UserHandle {
    const val USER_SYSTEM = 0
    var user = 0
    fun myUserId(): Int = user
}
class PowerManager(var isInteractive: Boolean = true)
class UserManager(var isUserUnlocked: Boolean = true)

object SystemClock {
    var now = 100_000L
    fun elapsedRealtime(): Long = now
    fun advanceBy(millis: Long) {
        now += millis
        Handler.runDue()
    }
}

class Looper private constructor() {
    companion object {
        private val main = Looper()
        fun getMainLooper(): Looper = main
    }
}

class Handler(looper: Looper) {
    fun postDelayed(task: Runnable, delay: Long): Boolean {
        pending += Scheduled(this, task, SystemClock.now + delay)
        return true
    }
    fun post(task: Runnable): Boolean = postDelayed(task, 0L)
    fun removeCallbacks(task: Runnable) { pending.removeAll { it.owner === this && it.task === task } }

    companion object {
        private data class Scheduled(val owner: Handler, val task: Runnable, val at: Long)
        private val pending = mutableListOf<Scheduled>()
        fun clear() = pending.clear()
        fun pendingCount(): Int = pending.size
        fun runDue() {
            while (true) {
                val next = pending.filter { it.at <= SystemClock.now }.minByOrNull { it.at } ?: return
                pending.remove(next)
                next.task.run()
            }
        }
    }
}
