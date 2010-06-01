/*
 * Copyright (c) 2006, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
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
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

import java.io.IOException;
import javax.xml.parsers.ParserConfigurationException;
import org.xml.sax.SAXException;
import com.sun.org.apache.xml.internal.security.c14n.*;
import com.sun.org.apache.xml.internal.security.exceptions.*;
import com.sun.org.apache.xml.internal.security.signature.XMLSignatureInput;
import com.sun.org.apache.xml.internal.security.transforms.*;

public class MyTransform extends TransformSpi {

    public static final String URI =
        "http://com.sun.org.apache.xml.internal.security.transforms.MyTransform";

    public MyTransform() {
        try {
            System.out.println("Registering Transform");
            Transform.init();
            Transform.register(URI, "MyTransform");
        } catch (AlgorithmAlreadyRegisteredException e) {
            // should not occur, so ignore
        }
    }

    protected String engineGetURI() {
        return URI;
    }

    protected XMLSignatureInput enginePerformTransform(XMLSignatureInput input)
        throws IOException, CanonicalizationException,
               InvalidCanonicalizerException, TransformationException,
               ParserConfigurationException, SAXException {
        throw new TransformationException("Unsupported Operation");
    }
}
