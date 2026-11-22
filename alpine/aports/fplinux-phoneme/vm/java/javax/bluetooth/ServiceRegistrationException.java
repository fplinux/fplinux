// SPDX-License-Identifier: GPL-2.0-only
package javax.bluetooth;

import java.io.IOException;

/** A service record could not be added to, changed in or removed from the service discovery database. */
public class ServiceRegistrationException extends IOException {
    public ServiceRegistrationException() {
    }

    public ServiceRegistrationException(String message) {
        super(message);
    }
}
