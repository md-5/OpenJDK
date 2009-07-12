/*
 * Copyright 2009 Sun Microsystems, Inc.  All Rights Reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Sun designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Sun in the LICENSE file that accompanied this code.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa Clara,
 * CA 95054 USA or visit www.sun.com if you need additional information or
 * have any questions.
 */

package sun.swing;

import sun.misc.Unsafe;

import java.awt.Point;

import javax.swing.text.JTextComponent;
import javax.swing.TransferHandler;

/**
 * The SwingAccessor utility class.
 * The main purpose of this class is to enable accessing
 * private and package-private fields of classes from
 * different classes/packages. See sun.misc.SharedSecretes
 * for another example.
 */
public final class SwingAccessor {
    private static final Unsafe unsafe = Unsafe.getUnsafe();

    /**
     * We don't need any objects of this class.
     * It's rather a collection of static methods
     * and interfaces.
     */
    private SwingAccessor() {
    }

    /**
     * An accessor for the JTextComponent class.
     * Note that we intentionally introduce the JTextComponentAccessor,
     * and not the JComponentAccessor because the needed methods
     * aren't override methods.
     */
    public interface JTextComponentAccessor {

        /**
         * Calculates a custom drop location for the text component,
         * representing where a drop at the given point should insert data.
         */
        TransferHandler.DropLocation dropLocationForPoint(JTextComponent textComp, Point p);

        /**
         * Called to set or clear the drop location during a DnD operation.
         */
        Object setDropLocation(JTextComponent textComp, TransferHandler.DropLocation location,
                               Object state, boolean forDrop);
    }

    /**
     * The javax.swing.text.JTextComponent class accessor object.
     */
    private static JTextComponentAccessor jtextComponentAccessor;

    /**
     * Set an accessor object for the javax.swing.text.JTextComponent class.
     */
    public static void setJTextComponentAccessor(JTextComponentAccessor jtca) {
         jtextComponentAccessor = jtca;
    }

    /**
     * Retrieve the accessor object for the javax.swing.text.JTextComponent class.
     */
    public static JTextComponentAccessor getJTextComponentAccessor() {
        if (jtextComponentAccessor == null) {
            unsafe.ensureClassInitialized(JTextComponent.class);
        }

        return jtextComponentAccessor;
    }
}
