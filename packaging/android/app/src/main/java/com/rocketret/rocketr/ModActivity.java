package com.rocketret.rocketr;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Typeface;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.widget.*;
import org.json.*;
import java.io.*;

/** Mod management is available before the SDL game process starts loading mods. */
public final class ModActivity extends Activity {
    private static final int PICK_MOD = 1002;
    private LinearLayout content;
    private String message = "";
    private boolean busy;
    static { System.loadLibrary("SDL2"); System.loadLibrary("main"); }
    static native String nativeCommand(String root, String request);

    private JSONObject command(JSONObject request) throws Exception {
        JSONObject answer = new JSONObject(nativeCommand(new File(getFilesDir(), "config").getPath(), request.toString()));
        if (!answer.getBoolean("ok")) throw new IOException(answer.getString("error"));
        return answer;
    }
    private void action(JSONObject request) {
        if (busy) return;
        try { command(request); message = "Saved. Changes apply on your next launch."; }
        catch (Exception e) { message = e.getMessage(); }
        showMods();
    }
    private JSONObject request(String operation) {
        JSONObject value = new JSONObject();
        try { value.put("op", operation); } catch (JSONException ignored) { }
        return value;
    }
    private TextView text(String value, float size) {
        TextView view = new TextView(this); view.setText(value); view.setTextSize(size);
        view.setTypeface(Typeface.create("Comic Sans MS", Typeface.NORMAL));
        view.setPadding(0, 12, 0, 12); content.addView(view); return view;
    }
    private void button(String label, Runnable callback) {
        Button view = new Button(this); view.setText(label);
        view.setTypeface(Typeface.create("Comic Sans MS", Typeface.BOLD));
        view.setOnClickListener(v -> callback.run()); content.addView(view);
    }
    @Override protected void onCreate(Bundle state) { super.onCreate(state); showMods(); }

    private void showMods() {
        ScrollView scroll = new ScrollView(this);
        content = new LinearLayout(this); content.setOrientation(LinearLayout.VERTICAL);
        int pad = (int)(20 * getResources().getDisplayMetrics().density);
        content.setPadding(pad,pad,pad,pad); scroll.addView(content); setContentView(scroll);
        text("MODS", 30); text("Add mods and choose how you want to play.", 18);
        button("BACK TO LAUNCHER", this::finish);
        if (!message.isEmpty()) text(message, 16);
        try {
            JSONObject snapshot = command(request("snapshot"));
            if (snapshot.getBoolean("recovery")) {
                text("The last session did not close normally. Your next launch will skip mods and use your original saves.", 18);
                button("TRY MY MODS AGAIN", () -> action(request("retry")));
            }
            JSONObject profile = snapshot.getJSONObject("profile");
            text("Profile: " + profile.getString("name"), 22);
            JSONArray profiles = snapshot.getJSONArray("profiles");
            for (int i=0;i<profiles.length();i++) {
                JSONObject p=profiles.getJSONObject(i);
                JSONObject select=request("select").put("id",p.getString("id"));
                button(p.getString("name"), () -> action(select));
            }
            text("Original Game uses your existing saves. Each mod profile keeps its own saves.",16);
            EditText name = new EditText(this); name.setHint("New profile name");
            name.setTypeface(Typeface.create("Comic Sans MS", Typeface.NORMAL));content.addView(name);
            button("CREATE PROFILE", () -> {
                try { action(request("create").put("name",name.getText().toString())); }
                catch(Exception e){message=e.getMessage();showMods();}
            });
            button("ADD MOD FILE", () -> {
                Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*");
                startActivityForResult(intent,PICK_MOD);
            });
            text("BROWSE",22);
            String cameraVersion = snapshot.getJSONObject("included_camera").getString("version");
            text("Modern Analogue Camera v" + cameraVersion + "\nIncluded with Rocket-R, by ThatGuyMcd.",18);
            text("Remap camera inputs in the game's Settings > Mods > Installed > Modern Analogue Camera > Details and settings. Touch controls include LOOK, ZOOM and VIEW while the mod controls the camera.",16);
            button("INSTALL / UPDATE CAMERA MOD v" + cameraVersion, () -> action(request("camera")));
            text("Code mods run inside the game. Choose packages from authors you trust.",16);
            text("INSTALLED",22);
            JSONArray errors=snapshot.getJSONArray("errors");
            for(int i=0;i<errors.length();i++)text(errors.getString(i),16);
            JSONArray packages=snapshot.getJSONArray("packages"), enabled=profile.getJSONArray("mods");
            java.util.HashSet<String> shown=new java.util.HashSet<>();
            for(int i=packages.length()-1;i>=0;i--) {
                JSONObject pkg=packages.getJSONObject(i);String id=pkg.getString("id");
                JSONObject entry=null;
                for(int j=0;j<enabled.length();j++)if(enabled.getJSONObject(j).getString("id").equals(id))entry=enabled.getJSONObject(j);
                if(entry!=null&&!entry.getString("hash").equals(pkg.getString("hash")))continue;
                if(!shown.add(id))continue;
                JSONObject manifest=pkg.getJSONObject("manifest");
                text(manifest.getString("display_name")+"  v"+manifest.getString("version"),22);
                text(manifest.optString("description"),16);
                Switch toggle=new Switch(this);toggle.setText("Enabled");toggle.setChecked(entry!=null&&entry.getBoolean("enabled"));
                content.addView(toggle);toggle.setOnCheckedChangeListener((v,checked)->{
                    try{action(request("enable").put("id",id).put("enabled",checked));}catch(Exception ignored){}
                });
                if(entry==null||!manifest.has("config_schema"))continue;
                JSONObject values=entry.getJSONObject("settings");
                JSONArray options=manifest.getJSONObject("config_schema").getJSONArray("options");
                for(int j=0;j<options.length();j++) {
                    JSONObject option=options.getJSONObject(j);String key=option.getString("id");
                    Object value=values.has(key)?values.get(key):option.get("default");
                    text(option.getString("name")+": "+value,18);
                    if(option.getString("type").equals("Number")) {
                        double min=option.getDouble("min"),max=option.getDouble("max");
                        SeekBar slider=new SeekBar(this);slider.setMax(100);
                        slider.setProgress((int)((((Number)value).doubleValue()-min)/(max-min)*100));content.addView(slider);
                        slider.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener(){
                            public void onStartTrackingTouch(SeekBar b){}
                            public void onProgressChanged(SeekBar b,int p,boolean user){}
                            public void onStopTrackingTouch(SeekBar b){try{action(request("option").put("id",id).put("key",key).put("value",min+(max-min)*b.getProgress()/100.0));}catch(Exception ignored){}}
                        });
                    } else if(option.getString("type").equals("Enum")) {
                        JSONArray choices=option.getJSONArray("options");
                        for(int c=0;c<choices.length();c++) {
                            String choice=choices.getString(c);
                            JSONObject change=request("option").put("id",id).put("key",key).put("value",choice);
                            button(choice,()->action(change));
                        }
                    }
                }
            }
        } catch(Exception e){text("Could not open mods: "+e.getMessage(),18);}
    }
    @Override protected void onActivityResult(int requestCode,int resultCode,Intent data) {
        super.onActivityResult(requestCode,resultCode,data);
        if(requestCode!=PICK_MOD||resultCode!=RESULT_OK||data==null||data.getData()==null)return;
        String name="mod.nrm";
        try(android.database.Cursor cursor=getContentResolver().query(data.getData(),null,null,null,null)) {
            if(cursor!=null&&cursor.moveToFirst())name=cursor.getString(cursor.getColumnIndexOrThrow(OpenableColumns.DISPLAY_NAME));
        }catch(Exception ignored){}
        String ext=name.substring(Math.max(0,name.lastIndexOf('.'))).toLowerCase(java.util.Locale.ROOT);
        if(!ext.equals(".nrm")&&!ext.equals(".rtz")&&!ext.equals(".zip")){message="Choose an .nrm, .rtz or .zip mod.";showMods();return;}
        busy=true;message="Importing mod…";showMods();
        final String extension=ext;
        new Thread(()->{
            File temporary=null;
            try {
                temporary=File.createTempFile("rocket-mod-",extension,getCacheDir());
                try(InputStream input=getContentResolver().openInputStream(data.getData());FileOutputStream output=new FileOutputStream(temporary)) {
                    if(input==null)throw new IOException("Could not open file.");
                    byte[] buffer=new byte[65536];long total=0;int count;
                    while((count=input.read(buffer))!=-1){total+=count;if(total>256L*1024*1024)throw new IOException("Package exceeds 256 MiB.");output.write(buffer,0,count);}
                }
                command(request("install").put("path",temporary.getPath()));message="Mod imported.";
            }catch(Exception e){message=e.getMessage();}
            finally{if(temporary!=null)temporary.delete();busy=false;runOnUiThread(this::showMods);}
        },"RocketModImport").start();
    }
}
