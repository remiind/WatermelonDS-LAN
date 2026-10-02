package me.magnum.melonds.ui.settings

import android.app.Activity
import android.app.Application
import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.Bundle
import android.os.Process
import me.magnum.melonds.ui.romlist.RomListActivity

class RestartAppActivity : Activity() {
    companion object {
        private const val SOURCE_PID = "source_pid"

        fun isRestartProcess(context: Context): Boolean =
            Build.VERSION.SDK_INT >= Build.VERSION_CODES.P &&
                Application.getProcessName() == "${context.packageName}:restart"

        fun restart(activity: Activity) {
            activity.startActivity(
                Intent(activity, RestartAppActivity::class.java)
                    .putExtra(SOURCE_PID, Process.myPid()),
            )
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val sourcePid = intent.getIntExtra(SOURCE_PID, -1)
        if (sourcePid > 0 && sourcePid != Process.myPid()) {
            Process.killProcess(sourcePid)
            startActivity(Intent.makeRestartActivityTask(
                android.content.ComponentName(this, RomListActivity::class.java),
            ))
        }
        finish()
        Process.killProcess(Process.myPid())
    }
}
