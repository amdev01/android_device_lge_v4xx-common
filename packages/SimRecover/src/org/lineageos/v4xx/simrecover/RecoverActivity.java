package org.lineageos.v4xx.simrecover;

import android.app.Activity;
import android.app.AlertDialog;
import android.os.Bundle;
import android.os.SystemProperties;
import android.widget.Toast;

/**
 * One-tap modem SSR for LTE v4xx tablets after SIM reinsert.
 * Sets sys.radio.modem_ssr=1; init.v4xx.rc writes restart to msm_subsys/modem.
 */
public class RecoverActivity extends Activity {
    private static final String PROP_SSR = "sys.radio.modem_ssr";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        new AlertDialog.Builder(this)
                .setTitle(R.string.title)
                .setMessage(R.string.message)
                .setPositiveButton(R.string.recover, (d, w) -> {
                    SystemProperties.set(PROP_SSR, "1");
                    Toast.makeText(this, R.string.triggered, Toast.LENGTH_LONG).show();
                    finish();
                })
                .setNegativeButton(R.string.cancel, (d, w) -> finish())
                .setOnCancelListener(d -> finish())
                .show();
    }
}
