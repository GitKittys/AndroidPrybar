package com.prybar.tracedemo;

import android.app.Activity;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.View;
import android.widget.Button;
import android.widget.TextView;

public class MainActivity extends Activity {
    private final Handler mainHandler = new Handler(Looper.getMainLooper());

    static {
        System.loadLibrary("trace");
        System.loadLibrary("tracedemo");
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        TextView outputView = findViewById(R.id.output_text);
        Button runButton = findViewById(R.id.run_button);

        outputView.setText(stringFromJNI());
        runButton.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View view) {
                runDemo(outputView);
            }
        });
    }

    private void runDemo(TextView outputView) {
        outputView.setText(getString(R.string.demo_running));
        new Thread(new Runnable() {
            @Override
            public void run() {
                final String result = runNativeDemo(getFilesDir().getAbsolutePath());
                mainHandler.post(new Runnable() {
                    @Override
                    public void run() {
                        outputView.setText(result);
                    }
                });
            }
        }, "trace-demo-thread").start();
    }

    public native String stringFromJNI();
    public native String runNativeDemo(String filesDir);
}
