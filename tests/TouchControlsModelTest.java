package com.rocketret.rocketr;

public final class TouchControlsModelTest {
    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }

    private static TouchControlsModel.Button button(TouchControlsModel model, int mask) {
        for (TouchControlsModel.Button value : model.buttons()) if (value.mask == mask) return value;
        throw new AssertionError("Missing button " + mask);
    }

    public static void main(String[] args) {
        TouchControlsModel model = new TouchControlsModel();
        for (float[] size : new float[][] {{900,480}, {2400,1080}, {1920,1920}, {800,360}}) {
            model.layout(size[0], size[1]);
            int all = 0;
            for (TouchControlsModel.Button value : model.buttons()) {
                check(value.x-value.radius >= 0 && value.y-value.radius >= 0 &&
                      value.x+value.radius <= size[0] && value.y+value.radius <= size[1], "Control outside display");
                model.down(91, value.x, value.y);
                check(model.mask() == value.mask, "Wrong button hit: " + value.label);
                model.up(91);
                check(model.mask() == 0, "Button stuck after release");
                all |= value.mask;
            }
            check(all == 0xff3f, "Missing N64 button");
        }
        model.layout(900, 480);
        TouchControlsModel.Button a = button(model, 0x8000), z = button(model, 0x2000);
        model.down(7, model.stickCenterX, model.stickCenterY);
        model.move(7, model.stickCenterX+1000, model.stickCenterY-1000);
        check(Math.abs(model.stickX*model.stickX+model.stickY*model.stickY-1) < 0.001, "Stick not radially clamped");
        check(model.stickY > 0, "Stick up polarity");
        model.down(23, a.x, a.y);
        model.down(64, z.x, z.y);
        check(model.mask() == 0xa000 && model.stickX > 0, "Move, jump and grab together");
        model.down(25, a.x, a.y);
        model.up(23);
        check(model.mask() == 0xa000, "One finger released another finger's button");
        model.up(7);
        check(model.stickX == 0 && model.stickY == 0 && model.mask() == 0xa000, "Stick release affected buttons");
        model.move(25, 450, 240);
        check(model.mask() == 0x2000, "Slide off button did not release");
        model.move(25, a.x, a.y);
        check(model.mask() == 0xa000, "Slide back did not press");
        model.clear();
        check(model.mask() == 0 && model.stickX == 0, "Cancel/focus loss left held input");
        model.down(99, 450, 240);
        model.move(99, a.x, a.y);
        check(model.mask() == 0, "Unowned pointer stole a control");
        model.down(4, a.x, a.y);
        model.layout(1600, 720);
        check(model.mask() == 0 && model.stickX == 0, "Rotation left held input");
        for (float[] size : new float[][] {{900,480}, {2400,1080}, {1920,1920}, {800,360}}) {
            model.layout(size[0], size[1], true);
            float lookX = size[0] - 98*model.scale, lookY = size[1] - 218*model.scale;
            for (TouchControlsModel.Button value : model.buttons()) {
                check(!value.label.startsWith("C"), "Hidden C button still receives touches");
                check(value.x-value.radius >= 0 && value.y-value.radius >= 0 &&
                      value.x+value.radius <= size[0] && value.y+value.radius <= size[1], "Camera control outside display");
                float dx = value.x-lookX, dy = value.y-lookY;
                check(Math.hypot(dx,dy) > value.radius+64*model.scale, "Button overlaps look stick: " + value.label);
                model.down(91, value.x, value.y);
                check(model.mask() == value.mask, "Wrong camera button hit: " + value.label);
                model.up(91);
            }
            TouchControlsModel.Button zoom = button(model, 4), view = button(model, 8);
            check(zoom.label.equals("ZOOM") && view.label.equals("VIEW"), "Camera modes need visible labels");
            model.down(1, model.stickCenterX, model.stickCenterY);
            model.move(1, model.stickCenterX+model.stickRadius, model.stickCenterY);
            model.down(2, zoom.x, zoom.y);
            model.down(3, view.x, view.y);
            check(model.mask() == 12 && model.stickX > 0, "Camera buttons must work while moving");
            model.up(2);
            check(model.mask() == 8, "Releasing zoom also released view");
            model.layout(size[0], size[1], false);
            check(model.mask() == 0 && model.stickX == 0 && button(model,8).label.equals("C▲"),
                "Leaving modern camera must clear held input and restore C buttons");
        }
        System.out.println("Touch controls: N64 and modern camera layouts, 4 screen shapes, multi-touch, ownership, slide, cancellation and resize PASS");
    }
}
