/*
 * Copyright (c) 1998, 2019, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
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

package jdk.javadoc.internal.doclets.toolkit.util;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.net.HttpURLConnection;
import java.net.MalformedURLException;
import java.net.URISyntaxException;
import java.net.URL;
import java.net.URLConnection;
import java.util.HashMap;
import java.util.Map;
import java.util.TreeMap;

import javax.lang.model.element.Element;
import javax.lang.model.element.ModuleElement;
import javax.lang.model.element.PackageElement;
import javax.tools.Diagnostic;
import javax.tools.Diagnostic.Kind;
import javax.tools.DocumentationTool;

import com.sun.tools.javac.code.Flags;
import com.sun.tools.javac.code.Symbol.ModuleSymbol;
import jdk.javadoc.doclet.Reporter;
import jdk.javadoc.internal.doclets.toolkit.BaseConfiguration;
import jdk.javadoc.internal.doclets.toolkit.Resources;

/**
 * Process and manage "-link" and "-linkoffline" to external packages. The
 * options "-link" and "-linkoffline" both depend on the fact that Javadoc now
 * generates "package-list"(lists all the packages which are getting
 * documented) file in the current or the destination directory, while
 * generating the documentation.
 *
 *  <p><b>This is NOT part of any supported API.
 *  If you write code that depends on this, you do so at your own risk.
 *  This code and its internal interfaces are subject to change or
 *  deletion without notice.</b>
 *
 * @author Atul M Dambalkar
 * @author Robert Field
 */
public class Extern {

    /**
     * Map element names onto Extern Item objects.
     * Lazily initialized.
     */
    private Map<String, Item> moduleItems = new HashMap<>();
    private Map<String, Map<String, Item>> packageItems = new HashMap<>();

    /**
     * The global configuration information for this run.
     */
    private final BaseConfiguration configuration;

    private final Resources resources;

    private final Utils utils;

    /**
     * True if we are using -linkoffline and false if -link is used instead.
     */
    private boolean linkoffline = false;

    /**
     * Stores the info for one external doc set
     */
    private class Item {

        /**
         * Element name, found in the "element-list" file in the {@link #path}.
         */
        final String elementName;

        /**
         * The URL or the directory path at which the element documentation will be
         * avaliable.
         */
        final DocPath path;

        /**
         * If given path is directory path then true else if it is a URL then false.
         */
        final boolean relative;

        /**
         * Constructor to build a Extern Item object and map it with the element name.
         * If the same element name is found in the map, then the first mapped
         * Item object or offline location will be retained.
         *
         * @param elementName Element name found in the "element-list" file.
         * @param path        URL or Directory path from where the "element-list"
         * file is picked.
         * @param relative    True if path is URL, false if directory path.
         */
        Item(String elementName, DocPath path, boolean relative) {
            this.elementName = elementName;
            this.path = path;
            this.relative = relative;
        }

        /**
         * String representation of "this" with elementname and the path.
         */
        @Override
        public String toString() {
            return elementName + (relative? " -> " : " => ") + path.getPath();
        }
    }

    public Extern(BaseConfiguration configuration) {
        this.configuration = configuration;
        this.resources = configuration.getResources();
        this.utils = configuration.utils;
    }

    /**
     * Determine if a element item is externally documented.
     *
     * @param element an Element.
     * @return true if the element is externally documented
     */
    public boolean isExternal(Element element) {
        if (packageItems.isEmpty()) {
            return false;
        }
        PackageElement pe = utils.containingPackage(element);
        if (pe.isUnnamed()) {
            return false;
        }

        return findElementItem(pe) != null;
    }

    /**
     * Determine if a element item is a module or not.
     *
     * @param elementName name of the element.
     * @return true if the element is a module
     */
    public boolean isModule(String elementName) {
        Item elem = moduleItems.get(elementName);
        return elem != null;
    }

    /**
     * Convert a link to be an external link if appropriate.
     *
     * @param element The element .
     * @param relativepath    The relative path.
     * @param filename    The link to convert.
     * @return if external return converted link else return null
     */
    public DocLink getExternalLink(Element element, DocPath relativepath, String filename) {
        return getExternalLink(element, relativepath, filename, null);
    }

    public DocLink getExternalLink(Element element, DocPath relativepath, String filename,
            String memberName) {
        Item fnd = findElementItem(element);
        if (fnd == null)
            return null;

        // The following is somewhat questionable since we are using DocPath
        // to contain external URLs!
        DocPath p = fnd.relative ?
                relativepath.resolve(fnd.path).resolve(filename) :
                fnd.path.resolve(filename);
        return new DocLink(p, "is-external=true", memberName);
    }

    /**
     * Build the extern element list from given URL or the directory path,
     * as specified with the "-link" flag.
     * Flag error if the "-link" or "-linkoffline" option is already used.
     *
     * @param url        URL or Directory path.
     * @param reporter   The <code>DocErrorReporter</code> used to report errors.
     * @return true if successful, false otherwise
     * @throws DocFileIOException if there is a problem reading a element list file
     */
    public boolean link(String url, Reporter reporter) throws DocFileIOException {
        return link(url, url, reporter, false);
    }

    /**
     * Build the extern element list from given URL or the directory path,
     * as specified with the "-linkoffline" flag.
     * Flag error if the "-link" or "-linkoffline" option is already used.
     *
     * @param url        URL or Directory path.
     * @param elemlisturl This can be another URL for "element-list" or ordinary
     *                   file.
     * @param reporter   The <code>DocErrorReporter</code> used to report errors.
     * @return true if successful, false otherwise
     * @throws DocFileIOException if there is a problem reading the element list file
     */
    public boolean link(String url, String elemlisturl, Reporter reporter) throws DocFileIOException {
        return link(url, elemlisturl, reporter, true);
    }

    /*
     * Build the extern element list from given URL or the directory path.
     * Flag error if the "-link" or "-linkoffline" option is already used.
     *
     * @param url        URL or Directory path.
     * @param elemlisturl This can be another URL for "element-list" or ordinary
     *                   file.
     * @param reporter   The <code>DocErrorReporter</code> used to report errors.
     * @param linkoffline True if -linkoffline is used and false if -link is used.
     * @return true if successful, false otherwise
     * @throws DocFileIOException if there is a problem reading the element list file
     */
    private boolean link(String url, String elemlisturl, Reporter reporter, boolean linkoffline)
                throws DocFileIOException {
        this.linkoffline = linkoffline;
        try {
            url = adjustEndFileSeparator(url);
            if (isUrl(elemlisturl)) {
                readElementListFromURL(url, toURL(adjustEndFileSeparator(elemlisturl)));
            } else {
                readElementListFromFile(url, DocFile.createFileForInput(configuration, elemlisturl));
            }
            return true;
        } catch (Fault f) {
            reporter.print(Diagnostic.Kind.ERROR, f.getMessage());
            return false;
        }
    }

    private class Fault extends Exception {
        private static final long serialVersionUID = 0;

        Fault(String msg, Exception cause) {
            super(msg, cause);
        }
    }

    /**
     * Get the Extern Item object associated with this element name.
     *
     * @param element Element
     */
    private Item findElementItem(Element element) {
        Item item = null;
        if (element instanceof ModuleElement) {
            item = moduleItems.get(utils.getModuleName((ModuleElement)element));
        }
        else if (element instanceof PackageElement) {
            PackageElement packageElement = (PackageElement)element;
            ModuleElement moduleElement = utils.containingModule(packageElement);
            Map<String, Item> pkgMap = packageItems.get(utils.getModuleName(moduleElement));
            item = (pkgMap != null) ? pkgMap.get(utils.getPackageName(packageElement)) : null;
            if (item == null && isAutomaticModule(moduleElement)) {
                pkgMap = packageItems.get(utils.getModuleName(null));
                if (pkgMap != null) {
                    item = pkgMap.get(utils.getPackageName(packageElement));
                }
            }
        }
        return item;
    }

    /**
     * If the URL or Directory path is missing end file separator, add that.
     */
    private String adjustEndFileSeparator(String url) {
        return url.endsWith("/") ? url : url + '/';
    }

    /**
     * Fetch the URL and read the "element-list" file.
     *
     * @param urlpath        Path to the elements.
     * @param elemlisturlpath URL or the path to the "element-list" file.
     */
    private void readElementListFromURL(String urlpath, URL elemlisturlpath) throws Fault {
        try {
            URL link = elemlisturlpath.toURI().resolve(DocPaths.ELEMENT_LIST.getPath()).toURL();
            try (InputStream in = open(link)) {
                readElementList(in, urlpath, false);
            }
        } catch (URISyntaxException | MalformedURLException exc) {
            throw new Fault(resources.getText("doclet.MalformedURL", elemlisturlpath.toString()), exc);
        } catch (IOException exc) {
            readAlternateURL(urlpath, elemlisturlpath);
        }
    }

    /**
     * Fetch the URL and read the "package-list" file.
     *
     * @param urlpath        Path to the packages.
     * @param elemlisturlpath URL or the path to the "package-list" file.
     */
    private void readAlternateURL(String urlpath, URL elemlisturlpath) throws Fault {
        try {
            URL link = elemlisturlpath.toURI().resolve(DocPaths.PACKAGE_LIST.getPath()).toURL();
            try (InputStream in = open(link)) {
                readElementList(in, urlpath, false);
            }
        } catch (URISyntaxException | MalformedURLException exc) {
            throw new Fault(resources.getText("doclet.MalformedURL", elemlisturlpath.toString()), exc);
        } catch (IOException exc) {
            throw new Fault(resources.getText("doclet.URL_error", elemlisturlpath.toString()), exc);
        }
    }

    /**
     * Read the "element-list" file which is available locally.
     *
     * @param path URL or directory path to the elements.
     * @param elemListPath Path to the local "element-list" file.
     * @throws Fault if an error occurs that can be treated as a warning
     * @throws DocFileIOException if there is a problem opening the element list file
     */
    private void readElementListFromFile(String path, DocFile elemListPath)
            throws Fault, DocFileIOException {
        DocFile file = elemListPath.resolve(DocPaths.ELEMENT_LIST);
        if (! (file.isAbsolute() || linkoffline)){
            file = file.resolveAgainst(DocumentationTool.Location.DOCUMENTATION_OUTPUT);
        }
        if (file.exists()) {
            readElementList(file, path);
        } else {
            DocFile file1 = elemListPath.resolve(DocPaths.PACKAGE_LIST);
            if (!(file1.isAbsolute() || linkoffline)) {
                file1 = file1.resolveAgainst(DocumentationTool.Location.DOCUMENTATION_OUTPUT);
            }
            if (file1.exists()) {
                readElementList(file1, path);
            } else {
                throw new Fault(resources.getText("doclet.File_error", file.getPath()), null);
            }
        }
    }

    private void readElementList(DocFile file, String path) throws Fault, DocFileIOException {
        try {
            if (file.canRead()) {
                boolean pathIsRelative
                        = !isUrl(path)
                        && !DocFile.createFileForInput(configuration, path).isAbsolute();
                readElementList(file.openInputStream(), path, pathIsRelative);
            } else {
                throw new Fault(resources.getText("doclet.File_error", file.getPath()), null);
            }
        } catch (IOException exc) {
            throw new Fault(resources.getText("doclet.File_error", file.getPath()), exc);
        }
    }

    /**
     * Read the file "element-list" and for each element name found, create
     * Extern object and associate it with the element name in the map.
     *
     * @param input     InputStream from the "element-list" file.
     * @param path     URL or the directory path to the elements.
     * @param relative Is path relative?
     * @throws IOException if there is a problem reading or closing the stream
     */
    private void readElementList(InputStream input, String path, boolean relative)
                         throws Fault, IOException {
        try (BufferedReader in = new BufferedReader(new InputStreamReader(input))) {
            String elemname;
            DocPath elempath;
            String moduleName = null;
            DocPath basePath  = DocPath.create(path);
            while ((elemname = in.readLine()) != null) {
                if (elemname.length() > 0) {
                    elempath = basePath;
                    if (elemname.startsWith(DocletConstants.MODULE_PREFIX)) {
                        moduleName = elemname.replace(DocletConstants.MODULE_PREFIX, "");
                        Item item = new Item(moduleName, elempath, relative);
                        moduleItems.put(moduleName, item);
                    } else {
                        DocPath pkgPath = DocPath.create(elemname.replace('.', '/'));
                        if (moduleName != null) {
                            elempath = elempath.resolve(DocPath.create(moduleName).resolve(pkgPath));
                        } else {
                            elempath = elempath.resolve(pkgPath);
                        }
                        checkLinkCompatibility(elemname, moduleName, path);
                        Item item = new Item(elemname, elempath, relative);
                        packageItems.computeIfAbsent(
                                moduleName == null ? DocletConstants.DEFAULT_ELEMENT_NAME : moduleName,
                                k -> new TreeMap<>())
                            .putIfAbsent(elemname, item); // first-one-wins semantics
                    }
                }
            }
        }
    }

    private void checkLinkCompatibility(String packageName, String moduleName, String path) throws Fault {
        PackageElement pe = utils.elementUtils.getPackageElement(packageName);
        if (pe != null) {
            ModuleElement me = (ModuleElement)pe.getEnclosingElement();
            if (me == null || me.isUnnamed()) {
                if (moduleName != null) {
                    throw new Fault(resources.getText("doclet.linkMismatch_PackagedLinkedtoModule",
                            path), null);
                }
            } else if (moduleName == null) {
                // suppress the error message in the case of automatic modules
                if (!isAutomaticModule(me)) {
                    throw new Fault(resources.getText("doclet.linkMismatch_ModuleLinkedtoPackage",
                            path), null);
                }
            }
        }
    }

    // The following should be replaced by a new method such as Elements.isAutomaticModule
    private boolean isAutomaticModule(ModuleElement me) {
        if (me == null) {
            return false;
        } else {
            ModuleSymbol msym = (ModuleSymbol) me;
            return (msym.flags() & Flags.AUTOMATIC_MODULE) != 0;
        }
    }

    public boolean isUrl (String urlCandidate) {
        try {
            new URL(urlCandidate);
            //No exception was thrown, so this must really be a URL.
            return true;
        } catch (MalformedURLException e) {
            //Since exception is thrown, this must be a directory path.
            return false;
        }
    }

    private URL toURL(String url) throws Fault {
        try {
            return new URL(url);
        } catch (MalformedURLException e) {
            throw new Fault(resources.getText("doclet.MalformedURL", url), e);
        }
    }

    /**
     * Open a stream to a URL, following a limited number of redirects
     * if necessary.
     *
     * @param url the URL
     * @return the stream
     * @throws IOException if an error occurred accessing the URL
     */
    private InputStream open(URL url) throws IOException {
        URLConnection conn = url.openConnection();

        boolean redir;
        int redirects = 0;
        InputStream in;

        do {
            // Open the input stream before getting headers,
            // because getHeaderField() et al swallow IOExceptions.
            in = conn.getInputStream();
            redir = false;

            if (conn instanceof HttpURLConnection) {
                HttpURLConnection http = (HttpURLConnection)conn;
                int stat = http.getResponseCode();
                // See:
                // https://developer.mozilla.org/en-US/docs/Web/HTTP/Status
                // https://en.wikipedia.org/wiki/List_of_HTTP_status_codes#3xx_Redirection
                switch (stat) {
                    case 300: // Multiple Choices
                    case 301: // Moved Permanently
                    case 302: // Found (previously Moved Temporarily)
                    case 303: // See Other
                    case 307: // Temporary Redirect
                    case 308: // Permanent Redirect
                        URL base = http.getURL();
                        String loc = http.getHeaderField("Location");
                        URL target = null;
                        if (loc != null) {
                            target = new URL(base, loc);
                        }
                        http.disconnect();
                        if (target == null || redirects >= 5) {
                            throw new IOException("illegal URL redirect");
                        }
                        redir = true;
                        conn = target.openConnection();
                        redirects++;
                }
            }
        } while (redir);

        if (!url.equals(conn.getURL())) {
            configuration.getReporter().print(Kind.WARNING,
                    resources.getText("doclet.urlRedirected", url, conn.getURL()));
        }

        return in;
    }
}
