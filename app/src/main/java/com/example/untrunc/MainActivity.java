package com.example.untrunc;

import androidx.appcompat.app.AppCompatActivity;

import android.content.Intent;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.util.Log;

import com.example.untrunc.databinding.ActivityMainBinding;

public class MainActivity extends AppCompatActivity {

    // Used to load the 'untrunc' library on application startup.
    static {
        System.loadLibrary("untrunc");

        System.loadLibrary("avcodec");
        System.loadLibrary("avfilter");
        System.loadLibrary("avformat");
        System.loadLibrary("avutil");
        System.loadLibrary("swresample");
        System.loadLibrary("swscale");
    }

    private ActivityMainBinding binding;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        binding = ActivityMainBinding.inflate(getLayoutInflater());
        setContentView(binding.getRoot());

        if (!Environment.isExternalStorageManager()) {
            Intent intent = new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION);
            startActivity(intent);
            return;
        }

        Log.e("untrunc","result>>>>>>>> "+stringFromJNI(
                "/sdcard/byteflow/learnffmpeg/av_20220508204644.mp4",
                "/sdcard/byteflow/learnffmpeg/av_20220509002321.mp4"));
    }

    /**
     * A native method that is implemented by the 'untrunc' native library,
     * which is packaged with this application.
     * @param ok
     * @param corrupt
     */
    public native int stringFromJNI(String ok, String corrupt);
}