// SPDX-License-Identifier: GPL-2.0-only
package fplinux.fixtures.files;

import java.io.DataInputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.util.Enumeration;
import javax.microedition.io.Connector;
import javax.microedition.io.file.FileConnection;
import javax.microedition.io.file.FileSystemRegistry;
import javax.microedition.io.file.IllegalModeException;
import javax.microedition.midlet.MIDlet;

public final class FileConnectionMIDlet extends MIDlet {
    private static final String NAME = "\u041f\u0440\u043e\u0431\u0430 level.txt";

    protected void startApp() {
        try {
            checkRoots();
        } catch (Throwable failure) {
            failure.printStackTrace();
        } finally {
            notifyDestroyed();
        }
    }

    protected void pauseApp() {
    }

    protected void destroyApp(boolean unconditional) {
    }

    private static void require(boolean condition, String label) {
        if (!condition) {
            throw new RuntimeException(label);
        }
    }

    private static void checkRoots() throws IOException {
        Enumeration roots = FileSystemRegistry.listRoots();
        require(roots.hasMoreElements() && "c:/".equals(roots.nextElement()), "local root");
        checkFiles("c:/");
        if (roots.hasMoreElements()) {
            require("e:/".equals(roots.nextElement()) && !roots.hasMoreElements(), "card root");
            checkFiles("e:/");
        } else {
            try {
                Connector.open("file:///e:/");
                throw new RuntimeException("card root opened without a data volume");
            } catch (IOException expected) {
            }
            System.out.println("PM-FILES e:/ absent");
        }
    }

    private static void checkFiles(String drive) throws IOException {
        String root = "file:///" + drive;
        String folderUrl = root + "levels/";
        String fileUrl = folderUrl + "%D0%9F%D1%80%D0%BE%D0%B1%D0%B0%20level.txt";
        FileConnection folder = (FileConnection) Connector.open(folderUrl);
        require("levels/".equals(folder.getName()), "missing directory name");
        try {
            folder.create();
            throw new RuntimeException("directory URL created a regular file");
        } catch (IOException expected) {
        }
        folder.mkdir();
        FileConnection file = (FileConnection) Connector.open(fileUrl);
        require(!file.exists(), "nonexistent file");
        file.create();
        OutputStream output = file.openOutputStream();
        output.write(new byte[] { 1, 2, 3, 4 });
        output.close();
        output = file.openOutputStream(2);
        output.write(new byte[] { 9, 8 });
        output.close();
        require(file.fileSize() == 4 && NAME.equals(file.getName()), "written file");
        require(fileUrl.equals(file.getURL()), "encoded file URL");
        Enumeration entries = folder.list("*.txt", false);
        require(entries.hasMoreElements() && NAME.equals(entries.nextElement()) &&
                !entries.hasMoreElements(), "entry with spaces and Unicode");
        require(folder.directorySize(false) == 4, "directory size");
        DataInputStream data = Connector.openDataInputStream(fileUrl);
        require(data.readInt() == 0x01020908 && data.read() == -1,
                "read stream retains descriptor after Connection.close");
        data.close();

        DataInputStream old = file.openDataInputStream();
        require(old.available() == 4 && old.skip(2) == 2 && old.read() == 9,
                "available byte count and seek");
        file.rename("renamed.txt");
        try {
            old.read();
            throw new RuntimeException("stream survived rename");
        } catch (IOException expected) {
        }
        file.truncate(2);
        require(file.fileSize() == 2 && "renamed.txt".equals(file.getName()),
                "truncated renamed file");
        file.delete();
        require(!file.exists(), "deleted file");
        file.close();
        try {
            file.openInputStream();
            throw new RuntimeException("closed connection reopened a stream");
        } catch (IOException expected) {
        }
        folder.delete();
        folder.close();

        FileConnection readOnly = (FileConnection) Connector.open(
            root + "disabled", Connector.READ);
        try {
            readOnly.create();
            throw new RuntimeException("read-only connection created a file");
        } catch (IllegalModeException expected) {
        } finally {
            readOnly.close();
        }
        try {
            Connector.open(root + "../outside");
            throw new RuntimeException("file path escaped the root");
        } catch (IllegalArgumentException expected) {
        }
        System.out.println("PM-FILES " + drive + " create list read rewrite rename truncate delete");
    }
}
