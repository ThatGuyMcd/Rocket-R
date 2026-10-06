package com.rocketret.rocketr;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.os.Build;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowInsets;

/** A lightweight native layer; it never routes gameplay touches through ImGui. */
public final class TouchControlsView extends View {
    private final TouchControlsModel model = new TouchControlsModel();
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF settings = new RectF();
    private final RectF visibility = new RectF();
    private final SharedPreferences preferences;
    private boolean shown, overlay, menuGesture;
    private float leftInset, rightInset, topInset, bottomInset;
    private int lookPointer = -1;
    private boolean cameraActive, recenter;
    private float lookX, lookY, lookCenterX, lookCenterY;
    private long lastLookTap;

    private final Runnable refresh = new Runnable() {
        @Override public void run() {
            boolean next = RocketActivity.nativeOverlayVisible();
            if (getContext() instanceof RocketActivity)
                ((RocketActivity)getContext()).updateGameFrameRate(next);
            boolean nextCamera = RocketActivity.nativeCameraActive();
            if (nextCamera != cameraActive) { cameraActive=nextCamera; layoutControls(); }
            if (overlay != next) {
                overlay = next;
                release();
                invalidate();
            }
            postDelayed(this, 100);
        }
    };

    public TouchControlsView(Context context) {
        super(context);
        preferences = context.getSharedPreferences("touch-controls", Context.MODE_PRIVATE);
        shown = preferences.getBoolean("visible", true);
        setFocusable(false);
        setContentDescription("N64 touch controls. Tap Settings to open the game settings.");
        setOnApplyWindowInsetsListener((view, insets) -> {
            leftInset = insets.getSystemWindowInsetLeft();
            rightInset = insets.getSystemWindowInsetRight();
            topInset = insets.getSystemWindowInsetTop();
            bottomInset = insets.getSystemWindowInsetBottom();
            if (Build.VERSION.SDK_INT >= 28 && insets.getDisplayCutout() != null) {
                leftInset = Math.max(leftInset, insets.getDisplayCutout().getSafeInsetLeft());
                rightInset = Math.max(rightInset, insets.getDisplayCutout().getSafeInsetRight());
                topInset = Math.max(topInset, insets.getDisplayCutout().getSafeInsetTop());
                bottomInset = Math.max(bottomInset, insets.getDisplayCutout().getSafeInsetBottom());
            }
            layoutControls();
            return insets;
        });
    }

    public void resume() {
        removeCallbacks(refresh);
        post(refresh);
    }

    public void pause() {
        removeCallbacks(refresh);
        menuGesture = false;
        release();
    }

    private void publish() {
        RocketActivity.nativeTouchState(model.mask(), model.stickX, model.stickY);
        RocketActivity.nativeTouchLook(lookX, lookY, recenter);
        invalidate();
    }

    public void release() {
        model.clear();
        lookPointer=-1;lookX=lookY=0;recenter=false;
        publish();
    }

    @Override protected void onDetachedFromWindow() {
        pause();
        super.onDetachedFromWindow();
    }

    @Override protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        layoutControls();
    }

    private void layoutControls() {
        menuGesture = false;
        float width = Math.max(1, getWidth() - leftInset - rightInset);
        float height = Math.max(1, getHeight() - topInset - bottomInset);
        model.layout(width, height, cameraActive);
        float s = model.scale;
        lookCenterX=width-98*s;lookCenterY=height-218*s;
        lookPointer=-1;lookX=lookY=0;recenter=false;
        settings.set(width / 2 + 6*s, 12*s, width / 2 + 130*s, 58*s);
        visibility.set(width / 2 - 130*s, 12*s, width / 2 - 6*s, 58*s);
        publish();
    }

    private void label(Canvas canvas, String text, float x, float y, float size) {
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(Color.WHITE);
        paint.setTextAlign(Paint.Align.CENTER);
        paint.setTextSize(size);
        canvas.drawText(text, x, y - (paint.ascent() + paint.descent()) / 2, paint);
    }

    private void pill(Canvas canvas, RectF rect, String text) {
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(Color.argb(210, 12, 41, 54));
        canvas.drawRoundRect(rect, 12*model.scale, 12*model.scale, paint);
        paint.setTextSize(15*model.scale);
        float textWidth = paint.measureText(text);
        float fittedSize = 15*model.scale * Math.min(1.0f,
            Math.max(1.0f, rect.width() - 12*model.scale) / Math.max(1.0f, textWidth));
        label(canvas, text, rect.centerX(), rect.centerY(), fittedSize);
    }

    @Override protected void onDraw(Canvas canvas) {
        canvas.save();
        canvas.translate(leftInset, topInset);
        float s = model.scale;
        // While settings are open, every touch belongs to the SDL settings UI.
        // Its RETURN TO GAME button and Android Back both close that overlay.
        if (!overlay) {
            pill(canvas, settings, "SETTINGS");
            pill(canvas, visibility, shown ? "HIDE CONTROLS" : "SHOW CONTROLS");
            if (shown) {
                for (TouchControlsModel.Button button : model.buttons()) {
                    boolean down = (model.mask() & button.mask) != 0;
                    paint.setStyle(Paint.Style.FILL);
                    paint.setColor(down ? Color.argb(215, 244, 130, 29) : Color.argb(95, 9, 34, 47));
                    canvas.drawCircle(button.x, button.y, button.radius, paint);
                    paint.setStyle(Paint.Style.STROKE);
                    paint.setStrokeWidth(1.5f*s);
                    paint.setColor(Color.argb(170, 222, 240, 247));
                    canvas.drawCircle(button.x, button.y, button.radius, paint);
                    label(canvas, button.label, button.x, button.y,
                          (button.label.length() > 2 ? 11 : 19)*s);
                }
                paint.setStyle(Paint.Style.FILL);
                paint.setColor(Color.argb(80, 9, 34, 47));
                canvas.drawCircle(model.stickCenterX, model.stickCenterY, model.stickRadius, paint);
                paint.setStyle(Paint.Style.STROKE);
                paint.setStrokeWidth(2*s);
                paint.setColor(Color.argb(160, 222, 240, 247));
                canvas.drawCircle(model.stickCenterX, model.stickCenterY, model.stickRadius, paint);
                paint.setStyle(Paint.Style.FILL);
                paint.setColor(Color.argb(165, 117, 207, 224));
                canvas.drawCircle(model.stickCenterX + model.stickX * model.stickRadius * 0.65f,
                                  model.stickCenterY - model.stickY * model.stickRadius * 0.65f, 26*s, paint);
                if(cameraActive) {
                    paint.setColor(Color.argb(95,9,34,47));
                    canvas.drawCircle(lookCenterX,lookCenterY,64*s,paint);
                    paint.setColor(Color.argb(165,117,207,224));
                    canvas.drawCircle(lookCenterX+lookX*42*s,lookCenterY+lookY*42*s,20*s,paint);
                    label(canvas,"LOOK",lookCenterX,lookCenterY-80*s,16*s);
                    label(canvas,"Double tap to recenter",lookCenterX,lookCenterY+82*s,10*s);
                }
            }
        }
        canvas.restore();
    }

    @Override public boolean onTouchEvent(MotionEvent event) {
        // Query directly as well as in refresh so the first settings gesture
        // cannot leak into gameplay during the 100 ms visibility refresh.
        boolean visible = RocketActivity.nativeOverlayVisible();
        if (visible && !menuGesture) {
            if (!overlay) { overlay = true; release(); }
            return false;
        }
        int action = event.getActionMasked();
        int index = event.getActionIndex();
        float x = event.getX(index) - leftInset, y = event.getY(index) - topInset;
        if ((action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) && !menuGesture) {
            menuGesture = settings.contains(x, y) || visibility.contains(x, y);
            if (settings.contains(x, y)) {
                release();
                RocketActivity.nativeToggleOverlay();
            } else if (visibility.contains(x, y)) {
                shown = !shown;
                preferences.edit().putBoolean("visible", shown).apply();
                release();
            } else if (!shown) {
                return false;
            }
        }
        if (menuGesture) {
            if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) menuGesture = false;
            return true;
        }
        if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
            float dx=x-lookCenterX,dy=y-lookCenterY;
            if(cameraActive&&lookPointer==-1&&dx*dx+dy*dy<=64*64*model.scale*model.scale) {
                lookPointer=event.getPointerId(index);
                recenter=event.getEventTime()-lastLookTap<300;lastLookTap=event.getEventTime();
                lookX=lookY=0;
            } else model.down(event.getPointerId(index), x, y);
        } else if (action == MotionEvent.ACTION_MOVE) {
            for (int i = 0; i < event.getPointerCount(); ++i) {
                if(event.getPointerId(i)==lookPointer) {
                    lookX=Math.max(-1,Math.min(1,(event.getX(i)-leftInset-lookCenterX)/(64*model.scale)));
                    lookY=Math.max(-1,Math.min(1,(event.getY(i)-topInset-lookCenterY)/(64*model.scale)));
                } else model.move(event.getPointerId(i), event.getX(i) - leftInset, event.getY(i) - topInset);
            }
        } else if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP) {
            model.up(event.getPointerId(index));
            if(event.getPointerId(index)==lookPointer){lookPointer=-1;lookX=lookY=0;recenter=false;}
        } else if (action == MotionEvent.ACTION_CANCEL) {
            model.clear();
            lookPointer=-1;lookX=lookY=0;recenter=false;
        }
        publish();
        return true;
    }
}
