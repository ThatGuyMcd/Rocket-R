package com.rocketret.rocketr;

import android.app.Activity;
import android.animation.ObjectAnimator;
import android.animation.ValueAnimator;
import android.content.Intent;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.view.Gravity;
import android.view.View;
import android.view.animation.LinearInterpolator;
import android.widget.Button;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;

public final class MainActivity extends Activity {
    private static final int PICK_ROM = 1001;
    private File privateRom;
    // ROCKET-R UI V36 ANDROID SPINNING BRAND
    private ObjectAnimator brandAnimator;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        privateRom = new File(getFilesDir(), "rocket.us.z64");
        showLauncher();
    }

    private void showLauncher() {
        if (brandAnimator != null) {
            brandAnimator.cancel();
            brandAnimator = null;
        }
        final LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.CENTER);
        final int pad = (int)(24.0f * getResources().getDisplayMetrics().density);
        root.setPadding(pad, pad, pad, pad);

        ImageView brand = new ImageView(this);
        brand.setImageResource(R.drawable.rocket_r_brand);
        brand.setAdjustViewBounds(true);
        brand.setScaleType(ImageView.ScaleType.CENTER_INSIDE);
        brand.setCameraDistance(8000.0f * getResources().getDisplayMetrics().density);
        final int brandSize = (int)(180.0f * getResources().getDisplayMetrics().density);
        LinearLayout.LayoutParams brandParams = new LinearLayout.LayoutParams(brandSize, brandSize);
        brandParams.bottomMargin = pad;
        root.addView(brand, brandParams);
        brandAnimator = ObjectAnimator.ofFloat(brand, View.ROTATION_Y, 0.0f, 360.0f);
        brandAnimator.setDuration(4800L);
        brandAnimator.setRepeatCount(ValueAnimator.INFINITE);
        brandAnimator.setInterpolator(new LinearInterpolator());
        brandAnimator.start();

        TextView title = new TextView(this);
        title.setText("ROCKET-R\nRocket: Robot on wheels Recompiled");
        title.setTextSize(34.0f);
        title.setTypeface(Typeface.create("Comic Sans MS", Typeface.BOLD));
        title.setGravity(Gravity.CENTER);
        root.addView(title);

        TextView note = new TextView(this);
        note.setText("Choose your unmodified US ROM of Rocket: Robot on Wheels. A copy is saved on this device for future play.");
        note.setTextSize(20.0f);
        note.setTypeface(Typeface.create("Comic Sans MS", Typeface.NORMAL));
        note.setGravity(Gravity.CENTER);
        note.setPadding(0, pad, 0, pad);
        root.addView(note);

        if (privateRom.isFile() && privateRom.length() > 0) {
            Button play = new Button(this);
            play.setText("Play Rocket-R");
            play.setTextSize(20.0f);
            play.setTypeface(Typeface.create("Comic Sans MS", Typeface.BOLD));
            play.setOnClickListener(v -> launchGame());
            root.addView(play);
        }

        Button choose = new Button(this);
        choose.setText(privateRom.isFile() ? "Replace ROM" : "Choose ROM");
        choose.setTextSize(20.0f);
        choose.setTypeface(Typeface.create("Comic Sans MS", Typeface.BOLD));
        choose.setOnClickListener(v -> chooseRom());
        root.addView(choose);
        Button mods = new Button(this);
        mods.setText("MODS");
        mods.setTextSize(20.0f);
        mods.setTypeface(Typeface.create("Comic Sans MS", Typeface.BOLD));
        mods.setOnClickListener(v -> startActivity(new Intent(this, ModActivity.class)));
        root.addView(mods);
        setContentView(root);
    }

    private void chooseRom() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        startActivityForResult(intent, PICK_ROM);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != PICK_ROM || resultCode != RESULT_OK || data == null || data.getData() == null) {
            return;
        }
        Uri uri = data.getData();
        try (InputStream input = getContentResolver().openInputStream(uri);
             FileOutputStream output = new FileOutputStream(privateRom, false)) {
            if (input == null) throw new IllegalStateException("Could not open the selected file.");
            byte[] buffer = new byte[1024 * 1024];
            int read;
            while ((read = input.read(buffer)) > 0) output.write(buffer, 0, read);
            output.flush();
            launchGame();
        } catch (Exception ex) {
            privateRom.delete();
            Toast.makeText(this, "Could not import the ROM: " + ex.getMessage(), Toast.LENGTH_LONG).show();
            showLauncher();
        }
    }

    private void launchGame() {
        Intent intent = new Intent(this, RocketActivity.class);
        startActivity(intent);
    }

    @Override
    protected void onDestroy() {
        if (brandAnimator != null) {
            brandAnimator.cancel();
            brandAnimator = null;
        }
        super.onDestroy();
    }
}
