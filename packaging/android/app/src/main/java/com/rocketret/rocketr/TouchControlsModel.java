package com.rocketret.rocketr;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/** Geometry and pointer ownership, independent of Android for regression tests. */
public final class TouchControlsModel {
    public static final class Button {
        public final String label;
        public final int mask;
        public final float x, y, radius;

        Button(String label, int mask, float x, float y, float radius) {
            this.label = label;
            this.mask = mask;
            this.x = x;
            this.y = y;
            this.radius = radius;
        }

        boolean contains(float px, float py) {
            float dx = px - x, dy = py - y;
            return dx * dx + dy * dy <= radius * radius;
        }
    }

    private final List<Button> buttons = new ArrayList<>();
    private final Map<Integer, Integer> held = new HashMap<>();
    private int stickPointer = -1;
    public float stickX, stickY;
    public float stickCenterX, stickCenterY, stickRadius, scale;

    public List<Button> buttons() { return buttons; }

    public void layout(float width, float height) {
        clear();
        buttons.clear();
        scale = Math.min(width / 900.0f, height / 480.0f);
        float s = scale;
        stickRadius = 64 * s;
        stickCenterX = 96 * s;
        stickCenterY = height - 112 * s;
        add("A", 0x8000, width - 52*s, height - 86*s, 31*s);
        add("B", 0x4000, width - 119*s, height - 49*s, 27*s);
        add("Z", 0x2000, 206*s, height - 50*s, 28*s);
        add("L", 0x0020, 38*s, 90*s, 25*s);
        add("R", 0x0010, width - 38*s, 90*s, 25*s);
        add("START", 0x1000, width / 2, height - 34*s, 27*s);
        // C buttons sit above A/B; the D-pad sits above the left thumbstick.
        float cx = width - 98*s, cy = height - 218*s;
        add("C▲", 0x0008, cx, cy - 48*s, 23*s);
        add("C▼", 0x0004, cx, cy + 48*s, 23*s);
        add("C◀", 0x0002, cx - 48*s, cy, 23*s);
        add("C▶", 0x0001, cx + 48*s, cy, 23*s);
        float dx = 98*s, dy = height - 282*s;
        add("▲", 0x0800, dx, dy - 44*s, 21*s);
        add("▼", 0x0400, dx, dy + 44*s, 21*s);
        add("◀", 0x0200, dx - 44*s, dy, 21*s);
        add("▶", 0x0100, dx + 44*s, dy, 21*s);
    }

    private void add(String label, int mask, float x, float y, float radius) {
        buttons.add(new Button(label, mask, x, y, radius));
    }

    private int hit(float x, float y) {
        for (Button button : buttons) if (button.contains(x, y)) return button.mask;
        return 0;
    }

    public void down(int id, float x, float y) {
        float dx = x - stickCenterX, dy = y - stickCenterY;
        if (stickPointer == -1 && dx*dx + dy*dy <= stickRadius*stickRadius) {
            stickPointer = id;
            move(id, x, y);
        } else if (hit(x, y) != 0) {
            held.put(id, hit(x, y));
        }
    }

    public void move(int id, float x, float y) {
        if (id == stickPointer) {
            float dx = (x - stickCenterX) / stickRadius;
            float dy = (stickCenterY - y) / stickRadius;
            float length = (float)Math.sqrt(dx*dx + dy*dy);
            float magnitude = Math.min(1, Math.max(0, (length - 0.10f) / 0.90f));
            stickX = length > 0 ? dx / length * magnitude : 0;
            stickY = length > 0 ? dy / length * magnitude : 0;
        } else if (held.containsKey(id)) {
            // Sliding between buttons is allowed; a finger outside a button
            // releases it without stealing another finger's held control.
            held.put(id, hit(x, y));
        }
    }

    public void up(int id) {
        held.remove(id);
        if (id == stickPointer) {
            stickPointer = -1;
            stickX = stickY = 0;
        }
    }

    public int mask() {
        int result = 0;
        for (int value : held.values()) result |= value;
        return result;
    }

    public void clear() {
        held.clear();
        stickPointer = -1;
        stickX = stickY = 0;
    }
}
