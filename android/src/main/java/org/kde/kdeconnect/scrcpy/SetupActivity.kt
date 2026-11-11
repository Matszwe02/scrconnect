/*
 * SPDX-FileCopyrightText: 2026 KDE Connect contributors
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */
package org.kde.kdeconnect.scrcpy

import android.Manifest
import android.content.Intent
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.kde.kdeconnect.KdeConnect
import org.kde.kdeconnect.scrcpy.SelfAdb.Step
import org.kde.kdeconnect.ui.compose.KdeTheme
import org.kde.kdeconnect_tp.R

class SetupActivity : AppCompatActivity() {
    private fun vendorHint(): Int? = when (Build.MANUFACTURER.lowercase()) {
        "xiaomi", "redmi", "poco" -> R.string.mirror_hint_xiaomi
        "oppo", "realme", "oneplus" -> R.string.mirror_hint_oppo
        else -> null
    }

    override fun onStop() {
        super.onStop()
        KdeConnect.getInstance().reloadAllDevicePlugins()
    }

    override fun onDestroy() {
        super.onDestroy()
        if (isFinishing) PairingNotification.cancel(this)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent { KdeTheme(this) { Scaffold { padding -> Content(Modifier.padding(padding)) } } }
    }

    @Composable
    private fun Content(modifier: Modifier) {
        var step by remember { mutableStateOf<Step?>(null) }
        var code by remember { mutableStateOf("") }
        var error by remember { mutableStateOf<String?>(null) }
        var busy by remember { mutableStateOf(false) }
        val scope = rememberCoroutineScope()
        val notificationPermission = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) {}

        LaunchedEffect(Unit) {
            if (Build.VERSION.SDK_INT >= 33) notificationPermission.launch(Manifest.permission.POST_NOTIFICATIONS)
            while (true) {
                val next = withContext(Dispatchers.IO) { SelfAdb.step(this@SetupActivity) }
                when (next) {
                    Step.PAIR -> PairingNotification.show(this@SetupActivity)
                    Step.DONE, Step.UNSUPPORTED -> PairingNotification.cancel(this@SetupActivity)
                    else -> {}
                }
                step = next
                delay(2000)
            }
        }

        Column(modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Text(stringResource(R.string.mirror_setup_title), style = MaterialTheme.typography.headlineSmall)
            val current = step ?: return@Column Text(stringResource(R.string.mirror_setup_checking))
            Text(stringResource(R.string.mirror_setup_intro))
            val checks = listOf(R.string.mirror_check_android, R.string.mirror_check_wifi, R.string.mirror_check_developer, R.string.mirror_check_wireless, R.string.mirror_check_paired)
            checks.forEachIndexed { i, label ->
                val done = current.ordinal > i
                Text((if (done) "✓  " else "○  ") + stringResource(label), color = if (done) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.outline)
            }
            Text(stringResource(current.text))
            val (button, settings) = when (current) {
                Step.CONNECT_WIFI -> R.string.mirror_open_wifi to Settings.ACTION_WIFI_SETTINGS
                Step.ENABLE_DEVELOPER_OPTIONS -> R.string.mirror_open_about to Settings.ACTION_DEVICE_INFO_SETTINGS
                Step.ENABLE_WIRELESS_DEBUGGING, Step.PAIR -> R.string.mirror_open_developer to Settings.ACTION_APPLICATION_DEVELOPMENT_SETTINGS
                else -> null to null
            }
            if (button != null) Button(onClick = { runCatching { startActivity(Intent(settings)) } }) { Text(stringResource(button)) }
            if (current == Step.DONE) vendorHint()?.let { Text(stringResource(it), style = MaterialTheme.typography.bodySmall) }
            if (current == Step.PAIR) {
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedTextField(
                        value = code,
                        onValueChange = { code = it.filter(Char::isDigit).take(6) },
                        label = { Text(stringResource(R.string.mirror_pairing_code_hint)) },
                        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.NumberPassword),
                        singleLine = true,
                        modifier = Modifier.weight(1f),
                    )
                    Button(enabled = code.length == 6 && !busy, onClick = {
                        busy = true
                        scope.launch {
                            error = withContext(Dispatchers.IO) { SelfAdb.pair(this@SetupActivity, code) }
                            busy = false
                        }
                    }) { Text(stringResource(R.string.mirror_pair)) }
                }
            }
            error?.let { Text(it, color = MaterialTheme.colorScheme.error) }
        }
    }
}
