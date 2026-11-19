// SPDX-License-Identifier: GPL-2.0-only
package com.sun.midp.io.j2me.file;

import com.sun.cldc.io.ConnectionBaseInterface;
import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.UnsupportedEncodingException;
import java.util.Enumeration;
import java.util.Vector;
import javax.microedition.io.Connection;
import javax.microedition.io.ConnectionNotFoundException;
import javax.microedition.io.Connector;
import javax.microedition.io.file.ConnectionClosedException;
import javax.microedition.io.file.FileConnection;
import javax.microedition.io.file.IllegalModeException;

public final class Protocol implements ConnectionBaseInterface, FileConnection {
    public static final int MY_FILES = 0;
    public static final int MEMORY_CARD = 1;

    private static final int MODE = 0;
    private static final int SIZE = 1;
    private static final int MODIFIED = 2;
    private static final int TOTAL = 3;
    private static final int AVAILABLE = 4;
    private static final int USED = 5;
    private static final int DIRECTORY = 4;
    private static final int READABLE = 1;
    private static final int WRITABLE = 2;

    private static final int CREATE = 0;
    private static final int MKDIR = 1;
    private static final int DELETE = 2;
    private static final int RENAME = 3;
    private static final int TRUNCATE = 4;
    private static final int SET_READABLE = 5;
    private static final int SET_WRITABLE = 6;

    private int root;
    private int mode;
    private boolean open;
    private boolean directoryName;
    private String host;
    private String path;
    private FileInput input;
    private FileOutput output;

    public synchronized Connection openPrim(String url, int access, boolean timeouts)
            throws IOException {
        if (!url.startsWith("//")) {
            throw new IllegalArgumentException("file URL");
        }
        int slash = url.indexOf('/', 2);
        if (slash < 0 || (slash != 2 &&
                !"localhost".equals(url.substring(2, slash).toLowerCase()))) {
            throw new IllegalArgumentException("file URL host");
        }
        host = url.substring(2, slash);
        if (url.length() < slash + 4 || url.charAt(slash + 2) != ':' ||
                url.charAt(slash + 3) != '/') {
            throw new IllegalArgumentException("file URL root");
        }
        char letter = Character.toLowerCase(url.charAt(slash + 1));
        if (letter != 'c' && letter != 'e') {
            throw new ConnectionNotFoundException("file root");
        }
        root = letter == 'c' ? MY_FILES : MEMORY_CARD;
        if (!rootAvailable(root)) {
            throw new IOException("file root unavailable");
        }
        path = decodePath(url.substring(slash + 4));
        // Keep the path component itself free of a directory's trailing slash.
        directoryName = path.length() == 0 || path.endsWith("/");
        if (path.endsWith("/")) {
            path = path.substring(0, path.length() - 1);
        }
        int start = 0;
        for (int i = 0; i < path.length(); i++) {
            if (path.charAt(i) == '/') {
                checkName(path.substring(start, i));
                start = i + 1;
            }
        }
        if (path.length() != 0) {
            checkName(path.substring(start));
        }
        mode = access;
        open = true;
        return this;
    }

    public static native boolean rootAvailable(int root);

    private static String decodePath(String value) {
        StringBuffer text = new StringBuffer(value.length());
        ByteArrayOutputStream escapes = new ByteArrayOutputStream();
        int index = 0;
        while (index < value.length()) {
            if (value.charAt(index) != '%') {
                text.append(value.charAt(index++));
                continue;
            }
            escapes.reset();
            do {
                if (index + 2 >= value.length()) {
                    throw new IllegalArgumentException("file URL escape");
                }
                int high = Character.digit(value.charAt(index + 1), 16);
                int low = Character.digit(value.charAt(index + 2), 16);
                if (high < 0 || low < 0) {
                    throw new IllegalArgumentException("file URL escape");
                }
                escapes.write(high * 16 + low);
                index += 3;
            } while (index < value.length() && value.charAt(index) == '%');
            try {
                text.append(new String(escapes.toByteArray(), "UTF-8"));
            } catch (UnsupportedEncodingException error) {
                throw new IllegalArgumentException(error.toString());
            }
        }
        return text.toString();
    }

    private static String encodePath(String value) {
        final String digits = "0123456789ABCDEF";
        StringBuffer text = new StringBuffer(value.length());
        byte[] utf8;
        try {
            utf8 = value.getBytes("UTF-8");
        } catch (UnsupportedEncodingException error) {
            throw new IllegalArgumentException(error.toString());
        }
        for (int i = 0; i < utf8.length; i++) {
            int c = utf8[i] & 255;
            if (c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' ||
                    c >= '0' && c <= '9' || "-_.~/".indexOf(c) >= 0) {
                text.append((char) c);
            } else {
                text.append('%').append(digits.charAt(c >>> 4))
                    .append(digits.charAt(c & 15));
            }
        }
        return text.toString();
    }

    private static void checkName(String name) {
        if (name.length() == 0 || ".".equals(name) || "..".equals(name) ||
                name.indexOf('/') >= 0 || name.indexOf('\\') >= 0 ||
                name.indexOf(0) >= 0) {
            throw new IllegalArgumentException("file name");
        }
    }

    private void checkAccess(int required) {
        if (!open) {
            throw new ConnectionClosedException();
        }
        if ((mode & required) != required) {
            throw new IllegalModeException();
        }
    }

    private void checkStreamAccess(int required) throws IOException {
        if (!open) {
            throw new IOException("file connection is closed");
        }
        checkAccess(required);
    }

    private String rootName() {
        return root == MY_FILES ? "c:/" : "e:/";
    }

    private long info(int field) {
        checkAccess(Connector.READ);
        return query0(root, path, field);
    }

    private long fileMode() {
        return info(MODE);
    }

    public synchronized boolean canRead() {
        long value = fileMode();
        return value >= 0 && (value & READABLE) != 0;
    }

    public synchronized boolean canWrite() {
        long value = fileMode();
        return value >= 0 && (value & WRITABLE) != 0;
    }

    public synchronized boolean exists() {
        return fileMode() >= 0;
    }

    public synchronized boolean isDirectory() {
        long value = fileMode();
        return value >= 0 && (value & DIRECTORY) != 0;
    }

    public synchronized boolean isHidden() {
        return fileMode() >= 0 && path.length() != 0 && getName().startsWith(".");
    }

    public synchronized long lastModified() {
        return Math.max(0L, info(MODIFIED));
    }

    public synchronized long fileSize() throws IOException {
        if (isDirectory()) {
            throw new IOException("file is a directory");
        }
        return info(SIZE);
    }

    public synchronized long totalSize() {
        return info(TOTAL);
    }

    public synchronized long availableSize() {
        return info(AVAILABLE);
    }

    public synchronized long usedSize() {
        return info(USED);
    }

    public synchronized String getName() {
        if (path.length() == 0) {
            return rootName();
        }
        String name = path.substring(path.lastIndexOf('/') + 1);
        long value = query0(root, path, MODE);
        boolean directory = value < 0 ? directoryName : (value & DIRECTORY) != 0;
        return directory ? name + "/" : name;
    }

    public synchronized String getPath() {
        if (path.length() == 0) {
            return "/";
        }
        int slash = path.lastIndexOf('/') + 1;
        return "/" + rootName() + path.substring(0, slash);
    }

    public synchronized String getURL() {
        return "file://" + host + "/" + rootName() + encodePath(path) +
            (path.length() != 0 && getName().endsWith("/") ? "/" : "");
    }

    public synchronized boolean isOpen() {
        return open;
    }

    public synchronized void close() {
        // Connector.openInputStream closes the connection before it returns
        // the stream. Existing streams own their descriptors.
        open = false;
    }

    public Enumeration list() throws IOException {
        return list("*", false);
    }

    public synchronized Enumeration list(String filter, boolean includeHidden)
            throws IOException {
        checkAccess(Connector.READ);
        if (filter.indexOf('/') >= 0) {
            throw new IllegalArgumentException("directory filter");
        }
        String[] entries = list0(root, path, includeHidden);
        Vector matched = new Vector(entries.length);
        for (int i = 0; i < entries.length; i++) {
            String entry = entries[i];
            String name = entry.endsWith("/") ?
                entry.substring(0, entry.length() - 1) : entry;
            if (matches(filter, name)) {
                matched.addElement(entry);
            }
        }
        return matched.elements();
    }

    private static boolean matches(String pattern, String value) {
        int p = 0;
        int v = 0;
        int star = -1;
        int backtrack = 0;
        while (v < value.length()) {
            if (p < pattern.length() && pattern.charAt(p) == '*') {
                star = p++;
                backtrack = v;
            } else if (p < pattern.length() &&
                       pattern.charAt(p) == value.charAt(v)) {
                p++;
                v++;
            } else if (star >= 0) {
                p = star + 1;
                v = ++backtrack;
            } else {
                return false;
            }
        }
        while (p < pattern.length() && pattern.charAt(p) == '*') {
            p++;
        }
        return p == pattern.length();
    }

    public synchronized long directorySize(boolean recursive) throws IOException {
        checkAccess(Connector.READ);
        return directorySize0(root, path, recursive);
    }

    public synchronized void create() throws IOException {
        checkAccess(Connector.WRITE);
        if (directoryName) {
            throw new IOException("directory name");
        }
        change(CREATE, null, 0);
    }

    public synchronized void mkdir() throws IOException {
        change(MKDIR, null, 0);
        directoryName = true;
    }

    public synchronized void delete() throws IOException {
        checkAccess(Connector.WRITE);
        closeStreams();
        change(DELETE, null, 0);
    }

    public synchronized void rename(String name) throws IOException {
        checkAccess(Connector.WRITE);
        checkName(name);
        closeStreams();
        change(RENAME, name, 0);
        path = path.substring(0, path.lastIndexOf('/') + 1) + name;
    }

    public void truncate(long offset) throws IOException {
        if (offset < 0) {
            throw new IllegalArgumentException("negative size");
        }
        change(TRUNCATE, null, offset);
    }

    public void setReadable(boolean readable) throws IOException {
        change(SET_READABLE, null, readable ? 1 : 0);
    }

    public void setWritable(boolean writable) throws IOException {
        change(SET_WRITABLE, null, writable ? 1 : 0);
    }

    public synchronized void setHidden(boolean hidden) throws IOException {
        checkAccess(Connector.WRITE);
        throw new IOException("hidden attribute is unsupported");
    }

    public synchronized void setFileConnection(String name) throws IOException {
        checkAccess(0);
        long value = query0(root, path, MODE);
        if (value < 0 || (value & DIRECTORY) == 0) {
            throw new IOException("file connection is not a directory");
        }
        String next;
        if ("..".equals(name)) {
            if (path.length() == 0) {
                throw new IOException("file root has no parent");
            }
            int slash = path.lastIndexOf('/');
            next = slash < 0 ? "" : path.substring(0, slash);
        } else {
            if (name.endsWith("/")) {
                name = name.substring(0, name.length() - 1);
            }
            checkName(name);
            next = path.length() == 0 ? name : path + "/" + name;
        }
        long nextMode = query0(root, next, MODE);
        if (nextMode < 0) {
            throw new IllegalArgumentException("file does not exist");
        }
        closeStreams();
        path = next;
        directoryName = (nextMode & DIRECTORY) != 0;
    }

    private synchronized void change(int operation, String argument, long value)
            throws IOException {
        checkAccess(Connector.WRITE);
        if (path.length() == 0) {
            throw new IOException("cannot modify a file root");
        }
        change0(root, path, operation, argument, value);
    }

    public synchronized InputStream openInputStream() throws IOException {
        checkStreamAccess(Connector.READ);
        if (input != null) {
            throw new IOException("input stream already open");
        }
        input = new FileInput(open0(root, path, false, 0));
        return input;
    }

    public DataInputStream openDataInputStream() throws IOException {
        return new DataInputStream(openInputStream());
    }

    public OutputStream openOutputStream() throws IOException {
        return openOutputStream(0);
    }

    public synchronized OutputStream openOutputStream(long offset) throws IOException {
        checkStreamAccess(Connector.WRITE);
        if (offset < 0) {
            throw new IllegalArgumentException("negative file offset");
        }
        if (output != null) {
            throw new IOException("output stream already open");
        }
        output = new FileOutput(open0(root, path, true, offset));
        return output;
    }

    public DataOutputStream openDataOutputStream() throws IOException {
        return new DataOutputStream(openOutputStream());
    }

    private void closeStreams() throws IOException {
        try {
            if (input != null) {
                input.close();
            }
        } finally {
            if (output != null) {
                output.close();
            }
        }
    }

    private static void checkBuffer(byte[] buffer, int offset, int count) {
        if (offset < 0 || count < 0 || offset > buffer.length ||
                count > buffer.length - offset) {
            throw new IndexOutOfBoundsException();
        }
    }

    private static void checkDescriptor(int descriptor) throws IOException {
        if (descriptor < 0) {
            throw new IOException("file stream is closed");
        }
    }

    private final class FileInput extends InputStream {
        private int descriptor;
        private final byte[] one = new byte[1];

        FileInput(int descriptor) {
            this.descriptor = descriptor;
        }

        public int read() throws IOException {
            synchronized (Protocol.this) {
                return read(one, 0, 1) == -1 ? -1 : one[0] & 255;
            }
        }

        public int read(byte[] target, int offset, int count)
                throws IOException {
            checkBuffer(target, offset, count);
            synchronized (Protocol.this) {
                checkDescriptor(descriptor);
                return count == 0 ? 0 : read0(descriptor, target, offset, count);
            }
        }

        public int available() throws IOException {
            synchronized (Protocol.this) {
                checkDescriptor(descriptor);
                return (int) Math.min(Integer.MAX_VALUE,
                                       remaining0(descriptor, 0));
            }
        }

        public long skip(long count) throws IOException {
            synchronized (Protocol.this) {
                checkDescriptor(descriptor);
                return count > 0 ? remaining0(descriptor, count) : 0;
            }
        }

        public void close() throws IOException {
            synchronized (Protocol.this) {
                if (descriptor >= 0) {
                    int previous = descriptor;
                    descriptor = -1;
                    input = null;
                    close0(previous);
                }
            }
        }
    }

    private final class FileOutput extends OutputStream {
        private int descriptor;
        private final byte[] one = new byte[1];

        FileOutput(int descriptor) {
            this.descriptor = descriptor;
        }

        public void write(int value) throws IOException {
            synchronized (Protocol.this) {
                one[0] = (byte) value;
                write(one, 0, 1);
            }
        }

        public void write(byte[] source, int offset, int count)
                throws IOException {
            checkBuffer(source, offset, count);
            synchronized (Protocol.this) {
                checkDescriptor(descriptor);
                if (count != 0) {
                    write0(descriptor, source, offset, count);
                }
            }
        }

        public void flush() throws IOException {
            synchronized (Protocol.this) {
                checkDescriptor(descriptor);
                flush0(descriptor);
            }
        }

        public void close() throws IOException {
            synchronized (Protocol.this) {
                if (descriptor >= 0) {
                    int previous = descriptor;
                    descriptor = -1;
                    output = null;
                    close0(previous);
                }
            }
        }
    }

    private static native long query0(int root, String path, int field);
    private static native String[] list0(int root, String path, boolean hidden)
        throws IOException;
    private static native long directorySize0(int root, String path, boolean recursive)
        throws IOException;
    private static native void change0(int root, String path, int operation,
        String argument, long value) throws IOException;
    private static native int open0(int root, String path, boolean write, long offset)
        throws IOException;
    private static native int read0(int descriptor, byte[] data, int offset, int count)
        throws IOException;
    private static native long remaining0(int descriptor, long skip)
        throws IOException;
    private static native void write0(int descriptor, byte[] data, int offset, int count)
        throws IOException;
    private static native void flush0(int descriptor) throws IOException;
    private static native void close0(int descriptor) throws IOException;
}
