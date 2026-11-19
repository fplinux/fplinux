// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.io.file;

import java.io.IOException;
import java.io.OutputStream;
import java.util.Enumeration;
import javax.microedition.io.StreamConnection;

public interface FileConnection extends StreamConnection {
    long availableSize();
    boolean canRead();
    boolean canWrite();
    void create() throws IOException;
    void delete() throws IOException;
    long directorySize(boolean recursive) throws IOException;
    boolean exists();
    long fileSize() throws IOException;
    String getName();
    String getPath();
    String getURL();
    boolean isDirectory();
    boolean isHidden();
    boolean isOpen();
    long lastModified();
    Enumeration list() throws IOException;
    Enumeration list(String filter, boolean includeHidden) throws IOException;
    void mkdir() throws IOException;
    OutputStream openOutputStream(long byteOffset) throws IOException;
    void rename(String newName) throws IOException;
    void setFileConnection(String fileName) throws IOException;
    void setHidden(boolean hidden) throws IOException;
    void setReadable(boolean readable) throws IOException;
    void setWritable(boolean writable) throws IOException;
    long totalSize();
    void truncate(long byteOffset) throws IOException;
    long usedSize();
}
