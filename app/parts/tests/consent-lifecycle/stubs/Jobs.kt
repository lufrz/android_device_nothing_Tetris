// SPDX-License-Identifier: Apache-2.0
package android.app.job

import android.content.ComponentName
import android.content.Context
import android.os.SystemClock

class JobInfo(val id: Int) {
    val uris = mutableListOf<String>()
    var minimumLatency = 0L
    class TriggerContentUri(val uri: String, flags: Int)
    class Builder(id: Int, component: ComponentName) {
        private val job = JobInfo(id)
        fun addTriggerContentUri(trigger: TriggerContentUri): Builder {
            job.uris += trigger.uri; return this
        }
        fun setTriggerContentUpdateDelay(delay: Long) = this
        fun setTriggerContentMaxDelay(delay: Long) = this
        fun setMinimumLatency(delay: Long): Builder { job.minimumLatency = delay; return this }
        fun setOverrideDeadline(delay: Long) = this
        fun build() = job
    }
}
class JobParameters
class JobScheduler {
    val jobs = mutableMapOf<Int, JobInfo>()
    val deadlines = mutableMapOf<Int, Long>()
    var namespace = ""
    var onSchedule: (() -> Unit)? = null
    var schedules = 0
    fun forNamespace(value: String): JobScheduler { namespace = value; return this }
    fun cancel(id: Int) { jobs.remove(id); deadlines.remove(id) }
    fun getPendingJob(id: Int): JobInfo? = jobs[id]
    fun schedule(job: JobInfo): Int {
        jobs[job.id] = job
        deadlines[job.id] = SystemClock.now + job.minimumLatency
        schedules++
        onSchedule?.invoke()
        return RESULT_SUCCESS
    }
    companion object { const val RESULT_SUCCESS = 1 }
}
abstract class JobService : Context() {
    abstract fun onStartJob(params: JobParameters): Boolean
    abstract fun onStopJob(params: JobParameters): Boolean
}
