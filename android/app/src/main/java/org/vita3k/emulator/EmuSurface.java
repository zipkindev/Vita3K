package org.vita3k.emulator;

import android.content.Context;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.View;

import org.libsdl.app.SDLSurface;
import org.vita3k.emulator.overlay.InputOverlay;

import java.util.HashSet;

public class EmuSurface extends SDLSurface {

    private InputOverlay mOverlay;
    private final HashSet<Integer> mOverlayPointers = new HashSet<>();

    public InputOverlay getmOverlay() {
        return mOverlay;
    }

    public EmuSurface(Context context){
        super(context);
        mOverlay = new InputOverlay(context);
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        setSurfaceStatus(true);
        super.surfaceCreated(holder);
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        setSurfaceStatus(false);
        super.surfaceDestroyed(holder);
    }

    @Override
    public boolean onTouch(View v, MotionEvent event) {
        final int action = event.getActionMasked();
        final boolean overlayConcerned = mOverlay.onTouch(v, event);
        final boolean editing = mOverlay.isInEditMode();

        if (action == MotionEvent.ACTION_DOWN)
            mOverlayPointers.clear();
        if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
            final int id = event.getPointerId(event.getActionIndex());
            if (editing || mOverlay.isPointerOwned(id))
                mOverlayPointers.add(id);
            else
                mOverlayPointers.remove(id);
        }

        boolean handled = true;
        if (!editing) {
            final int pointerCount = event.getPointerCount();
            final boolean[] owned = new boolean[pointerCount];
            boolean anyOwned = false;
            for (int i = 0; i < pointerCount; i++) {
                owned[i] = mOverlayPointers.contains(event.getPointerId(i));
                anyOwned = anyOwned || owned[i];
            }

            if (!anyOwned) {
                handled = super.onTouch(v, event) || overlayConcerned;
            } else {
                final MotionEvent filtered = filterOwnedPointers(event, owned);
                if (filtered != null) {
                    handled = super.onTouch(v, filtered) || overlayConcerned;
                    filtered.recycle();
                }
            }
        }

        if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL)
            mOverlayPointers.clear();
        else if (action == MotionEvent.ACTION_POINTER_UP)
            mOverlayPointers.remove(event.getPointerId(event.getActionIndex()));
        return handled;
    }

    private MotionEvent filterOwnedPointers(MotionEvent event, boolean[] owned) {
        final int pointerCount = event.getPointerCount();
        int keptCount = 0;
        final int[] keptIndex = new int[pointerCount];
        for (int i = 0; i < pointerCount; i++) {
            if (!owned[i])
                keptIndex[keptCount++] = i;
        }
        if (keptCount == 0)
            return null;

        final MotionEvent.PointerProperties[] props = new MotionEvent.PointerProperties[keptCount];
        final MotionEvent.PointerCoords[] coords = new MotionEvent.PointerCoords[keptCount];
        final int actionIndex = event.getActionIndex();
        int newActionIndex = -1;
        for (int k = 0; k < keptCount; k++) {
            props[k] = new MotionEvent.PointerProperties();
            coords[k] = new MotionEvent.PointerCoords();
            event.getPointerProperties(keptIndex[k], props[k]);
            event.getPointerCoords(keptIndex[k], coords[k]);
            if (keptIndex[k] == actionIndex)
                newActionIndex = k;
        }

        int action;
        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_POINTER_DOWN:
                if (newActionIndex < 0)
                    action = MotionEvent.ACTION_MOVE; // an overlay pointer went down; kept ones just continue
                else if (keptCount == 1)
                    action = MotionEvent.ACTION_DOWN;
                else
                    action = MotionEvent.ACTION_POINTER_DOWN
                            | (newActionIndex << MotionEvent.ACTION_POINTER_INDEX_SHIFT);
                break;
            case MotionEvent.ACTION_POINTER_UP:
                if (newActionIndex < 0)
                    action = MotionEvent.ACTION_MOVE;
                else if (keptCount == 1)
                    action = MotionEvent.ACTION_UP;
                else
                    action = MotionEvent.ACTION_POINTER_UP
                            | (newActionIndex << MotionEvent.ACTION_POINTER_INDEX_SHIFT);
                break;
            default:
                // DOWN/UP/MOVE/CANCEL keep their meaning for the surviving pointers
                action = event.getActionMasked();
                break;
        }

        return MotionEvent.obtain(event.getDownTime(), event.getEventTime(), action, keptCount,
                props, coords, event.getMetaState(), event.getButtonState(), event.getXPrecision(),
                event.getYPrecision(), event.getDeviceId(), event.getEdgeFlags(), event.getSource(),
                event.getFlags());
    }

    public native void setSurfaceStatus(boolean surface_present);
}
