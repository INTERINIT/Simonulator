package org.interinit.simonulator;

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.os.Bundle;
import android.text.InputType;
import android.widget.*;
import java.io.*;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.zip.CRC32;

public class SetupActivity extends Activity {
    private static final int BIOS = 1, FLASH = 2, MICROPHONE = 3;
    private EditText address, port;
    private TextView status;
    private Button start;
    private File romDir;
    private SharedPreferences settings;

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        settings = getSharedPreferences("switch", MODE_PRIVATE);
        romDir = new File(getFilesDir(), "roms/ibmsimon");
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        int padding = Math.round(20 * getResources().getDisplayMetrics().density);
        layout.setPadding(padding, padding, padding, padding);
        ScrollView scroll = new ScrollView(this);
        scroll.setFitsSystemWindows(true);
        scroll.addView(layout);
        setContentView(scroll);
        label(layout, "Simonulator").setTextSize(28);
        label(layout, "交换机地址");
        address = new EditText(this);
        address.setSingleLine();
        address.setHint("192.168.1.10");
        address.setText(settings.getString("address", ""));
        layout.addView(address);
        label(layout, "端口");
        port = new EditText(this);
        port.setInputType(InputType.TYPE_CLASS_NUMBER);
        port.setText(settings.getString("port", "5555"));
        layout.addView(port);
        button(layout, "导入 BIOS", () -> select(BIOS));
        button(layout, "导入 1 MB Flash ROM", () -> select(FLASH));
        status = label(layout, "");
        start = button(layout, "启动 Simon", this::startSimon);
        updateStatus();
    }

    private TextView label(LinearLayout layout, String value) {
        TextView text = new TextView(this);
        text.setText(value);
        layout.addView(text);
        return text;
    }

    private Button button(LinearLayout layout, String name, Runnable action) {
        Button button = new Button(this);
        button.setText(name);
        button.setOnClickListener(v -> action.run());
        layout.addView(button);
        return button;
    }

    private void updateStatus() {
        boolean bios = new File(romDir, "simonbios.bin").isFile();
        boolean flash = new File(romDir, "simonflash.bin").isFile();
        status.setText("BIOS " + (bios ? "✓" : "待导入") + "    Flash " + (flash ? "✓" : "待导入"));
        start.setEnabled(bios && flash);
    }

    private void select(int which) {
        startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT)
                .addCategory(Intent.CATEGORY_OPENABLE).setType("*/*"), which);
    }

    @Override protected void onActivityResult(int which, int result, Intent data) {
        super.onActivityResult(which, result, data);
        if (result != RESULT_OK || data == null || (which != BIOS && which != FLASH)) return;
        File temporary = null;
        try {
            Files.createDirectories(romDir.toPath());
            temporary = File.createTempFile("import-", ".part", romDir);
            CRC32 crc = new CRC32();
            long size = 0, expected = which == BIOS ? 0x20000 : 0x100000;
            try (InputStream in = getContentResolver().openInputStream(data.getData());
                 OutputStream out = new FileOutputStream(temporary)) {
                if (in == null) throw new IOException("无法打开文件");
                byte[] chunk = new byte[16384];
                int count;
                while ((count = in.read(chunk)) != -1) {
                    size += count;
                    if (size > expected) throw new IOException("文件过大");
                    out.write(chunk, 0, count);
                    crc.update(chunk, 0, count);
                }
            }
            long checksum = which == BIOS ? 0xe47b0e90L : 0x00f5a976L;
            if (size != expected || crc.getValue() != checksum) throw new IOException("所选 ROM 与 Simon 驱动的长度或校验值不符");
            File target = new File(romDir, which == BIOS ? "simonbios.bin" : "simonflash.bin");
            Files.move(temporary.toPath(), target.toPath(), StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
            updateStatus();
        } catch (IOException e) {
            status.setText(e.getMessage());
        } finally {
            if (temporary != null) temporary.delete();
        }
    }

    private void startSimon() {
        String host = address.getText().toString().trim(), value = port.getText().toString().trim();
        if (!host.isEmpty() && !host.matches("[A-Za-z0-9][A-Za-z0-9.-]{0,252}")) {
            address.setError("请输入交换机的 IPv4 地址或主机名");
            return;
        }
        int number;
        try { number = Integer.parseInt(value); } catch (NumberFormatException e) { number = 0; }
        if (number < 1 || number > 65535) { port.setError("有效范围为 1 到 65535"); return; }
        settings.edit().putString("address", host).putString("port", value).apply();
        if (!host.isEmpty() && checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED)
            requestPermissions(new String[]{Manifest.permission.RECORD_AUDIO}, MICROPHONE);
        else launch();
    }

    @Override public void onRequestPermissionsResult(int request, String[] permissions, int[] results) {
        super.onRequestPermissionsResult(request, permissions, results);
        if (request == MICROPHONE) launch();
    }

    private void launch() {
        // Pass the current values explicitly: the SDL activity has its own process.
        Intent intent = new Intent(this, SimonActivity.class);
        intent.putExtra("host", address.getText().toString().trim());
        intent.putExtra("port", port.getText().toString().trim());
        startActivity(intent);
    }
}
