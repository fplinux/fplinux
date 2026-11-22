// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.m3g;

import java.util.Hashtable;
import java.util.Vector;

public abstract class Object3D {
    private final Vector tracks = new Vector();
    private int userId;
    private Object userObject;

    public int getUserID() {
        return userId;
    }

    public void setUserID(int id) {
        userId = id;
    }

    public Object getUserObject() {
        return userObject;
    }

    public void setUserObject(Object value) {
        userObject = value;
    }

    public int getReferences(Object3D[] result) {
        Vector objects = new Vector();
        references(objects);
        if (result != null) {
            if (result.length < objects.size()) throw new IllegalArgumentException("references");
            for (int i = 0; i < objects.size(); i++) result[i] = (Object3D) objects.elementAt(i);
        }
        return objects.size();
    }

    public Object3D find(int id) {
        return find(id, new Hashtable());
    }

    private Object3D find(int id, Hashtable visited) {
        if (id == userId) return this;
        if (visited.put(this, this) != null) return null;
        Vector objects = new Vector();
        references(objects);
        for (int i = 0; i < objects.size(); i++) {
            Object3D found = ((Object3D) objects.elementAt(i)).find(id, visited);
            if (found != null) return found;
        }
        return null;
    }

    void references(Vector objects) {
        for (int i = 0; i < tracks.size(); i++) objects.addElement(tracks.elementAt(i));
    }

    static void addReference(Vector objects, Object3D value) {
        if (value != null) objects.addElement(value);
    }

    public void addAnimationTrack(AnimationTrack track) {
        if (track == null) {
            throw new NullPointerException();
        }
        if (!tracks.contains(track)) {
            tracks.addElement(track);
        }
    }

    public void removeAnimationTrack(AnimationTrack track) {
        tracks.removeElement(track);
    }

    public int getAnimationTrackCount() {
        return tracks.size();
    }

    public AnimationTrack getAnimationTrack(int index) {
        return (AnimationTrack) tracks.elementAt(index);
    }

    public int animate(int time) {
        return animate(time, new Hashtable());
    }

    private int animate(int time, Hashtable visited) {
        if (visited.put(this, this) != null) return Integer.MAX_VALUE;
        int nextChange = Integer.MAX_VALUE;
        for (int index = 0; index < tracks.size(); index++) {
            AnimationTrack track = getAnimationTrack(index);
            AnimationController controller = track.getController();
            if (controller == null || controller.getWeight() == 0) {
                continue;
            }
            int from = controller.getActiveIntervalStart();
            int until = controller.getActiveIntervalEnd();
            if (from != until && time < from) {
                nextChange = Math.min(nextChange,
                    (int) Math.min(Integer.MAX_VALUE, (long) from - time));
            } else if (from == until || time < until) {
                animated(track.property, track.sequence.sample(controller.getPosition(time)));
                nextChange = Math.min(nextChange, 1);
            }
        }
        Vector objects = new Vector();
        references(objects);
        for (int i = 0; i < objects.size(); i++) {
            nextChange = Math.min(nextChange, ((Object3D) objects.elementAt(i)).animate(time, visited));
        }
        return nextChange;
    }

    void animated(int property, float[] values) {
        throw new IllegalArgumentException("animation property for object");
    }

    public final Object3D duplicate() {
        return duplicate(new Hashtable());
    }

    final Object3D duplicate(Hashtable copies) {
        Object3D copy = (Object3D) copies.get(this);
        if (copy == null) {
            copy = newCopy();
            copies.put(this, copy);
            copyFields(copy, copies);
        }
        return copy;
    }

    abstract Object3D newCopy();

    void copyFields(Object3D copy, Hashtable copies) {
        copy.userId = userId;
        copy.userObject = userObject;
        for (int i = 0; i < tracks.size(); i++) {
            copy.tracks.addElement(tracks.elementAt(i));
        }
    }
}
