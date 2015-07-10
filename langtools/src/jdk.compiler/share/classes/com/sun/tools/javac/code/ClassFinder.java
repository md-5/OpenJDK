/*
 * Copyright (c) 1999, 2015, Oracle and/or its affiliates. All rights reserved.
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

package com.sun.tools.javac.code;

import java.io.IOException;
import java.io.File;
import java.util.EnumSet;
import java.util.HashMap;
import java.util.Map;
import java.util.Set;

import javax.lang.model.SourceVersion;
import javax.tools.JavaFileManager;
import javax.tools.JavaFileManager.Location;
import javax.tools.JavaFileObject;
import javax.tools.StandardJavaFileManager;

import com.sun.tools.javac.code.Scope.WriteableScope;
import com.sun.tools.javac.code.Symbol.ClassSymbol;
import com.sun.tools.javac.code.Symbol.Completer;
import com.sun.tools.javac.code.Symbol.CompletionFailure;
import com.sun.tools.javac.code.Symbol.PackageSymbol;
import com.sun.tools.javac.code.Symbol.TypeSymbol;
import com.sun.tools.javac.comp.Annotate;
import com.sun.tools.javac.comp.Enter;
import com.sun.tools.javac.file.JRTIndex;
import com.sun.tools.javac.file.JavacFileManager;
import com.sun.tools.javac.jvm.ClassReader;
import com.sun.tools.javac.jvm.Profile;
import com.sun.tools.javac.platform.PlatformDescription;
import com.sun.tools.javac.util.*;

import static javax.tools.StandardLocation.*;

import static com.sun.tools.javac.code.Flags.*;
import static com.sun.tools.javac.code.Kinds.Kind.*;

import static com.sun.tools.javac.main.Option.*;

import com.sun.tools.javac.util.Dependencies.CompletionCause;

/**
 *  This class provides operations to locate class definitions
 *  from the source and class files on the paths provided to javac.
 *
 *  <p><b>This is NOT part of any supported API.
 *  If you write code that depends on this, you do so at your own risk.
 *  This code and its internal interfaces are subject to change or
 *  deletion without notice.</b>
 */
public class ClassFinder {
    /** The context key for the class finder. */
    protected static final Context.Key<ClassFinder> classFinderKey = new Context.Key<>();

    ClassReader reader;

    private final Annotate annotate;

    /** Switch: verbose output.
     */
    boolean verbose;

    /**
     * Switch: cache completion failures unless -XDdev is used
     */
    private boolean cacheCompletionFailure;

    /**
     * Switch: prefer source files instead of newer when both source
     * and class are available
     **/
    protected boolean preferSource;

    /**
     * Switch: Search classpath and sourcepath for classes before the
     * bootclasspath
     */
    protected boolean userPathsFirst;

    /**
     * Switch: should read OTHER classfiles (.sig files) from PLATFORM_CLASS_PATH.
     */
    private boolean allowSigFiles;

    /** The log to use for verbose output
     */
    final Log log;

    /** The symbol table. */
    Symtab syms;

    /** The name table. */
    final Names names;

    /** Force a completion failure on this name
     */
    final Name completionFailureName;

    /** Access to files
     */
    private final JavaFileManager fileManager;

    /** Dependency tracker
     */
    private final Dependencies dependencies;

    /** Factory for diagnostics
     */
    JCDiagnostic.Factory diagFactory;

    /** Can be reassigned from outside:
     *  the completer to be used for ".java" files. If this remains unassigned
     *  ".java" files will not be loaded.
     */
    public Completer sourceCompleter = Completer.NULL_COMPLETER;

    /** The path name of the class file currently being read.
     */
    protected JavaFileObject currentClassFile = null;

    /** The class or method currently being read.
     */
    protected Symbol currentOwner = null;

    /**
     * The currently selected profile.
     */
    private final Profile profile;

    /**
     * Use direct access to the JRTIndex to access the temporary
     * replacement for the info that used to be in ct.sym.
     * In time, this will go away and be replaced by the module system.
     */
    private final JRTIndex jrtIndex;

    /**
     * Completer that delegates to the complete-method of this class.
     */
    private final Completer thisCompleter = new Completer() {
        @Override
        public void complete(Symbol sym) throws CompletionFailure {
            ClassFinder.this.complete(sym);
        }
    };

    public Completer getCompleter() {
        return thisCompleter;
    }

    /** Get the ClassFinder instance for this invocation. */
    public static ClassFinder instance(Context context) {
        ClassFinder instance = context.get(classFinderKey);
        if (instance == null)
            instance = new ClassFinder(context);
        return instance;
    }

    /** Construct a new class reader. */
    protected ClassFinder(Context context) {
        context.put(classFinderKey, this);
        reader = ClassReader.instance(context);
        names = Names.instance(context);
        syms = Symtab.instance(context);
        fileManager = context.get(JavaFileManager.class);
        dependencies = Dependencies.instance(context);
        if (fileManager == null)
            throw new AssertionError("FileManager initialization error");
        diagFactory = JCDiagnostic.Factory.instance(context);

        log = Log.instance(context);
        annotate = Annotate.instance(context);

        Options options = Options.instance(context);
        verbose = options.isSet(VERBOSE);
        cacheCompletionFailure = options.isUnset("dev");
        preferSource = "source".equals(options.get("-Xprefer"));
        userPathsFirst = options.isSet(XXUSERPATHSFIRST);
        allowSigFiles = context.get(PlatformDescription.class) != null;

        completionFailureName =
            options.isSet("failcomplete")
            ? names.fromString(options.get("failcomplete"))
            : null;

        // Temporary, until more info is available from the module system.
        boolean useCtProps;
        JavaFileManager fm = context.get(JavaFileManager.class);
        if (fm instanceof JavacFileManager) {
            JavacFileManager jfm = (JavacFileManager) fm;
            useCtProps = jfm.isDefaultBootClassPath() && jfm.isSymbolFileEnabled();
        } else if (fm.getClass().getName().equals("com.sun.tools.sjavac.comp.SmartFileManager")) {
            useCtProps = !options.isSet("ignore.symbol.file");
        } else {
            useCtProps = false;
        }
        jrtIndex = useCtProps && JRTIndex.isAvailable() ? JRTIndex.getSharedInstance() : null;

        profile = Profile.instance(context);
    }


/************************************************************************
 * Temporary ct.sym replacement
 *
 * The following code is a temporary substitute for the ct.sym mechanism
 * used in JDK 6 thru JDK 8.
 * This mechanism will eventually be superseded by the Jigsaw module system.
 ***********************************************************************/

    /**
     * Returns any extra flags for a class symbol.
     * This information used to be provided using private annotations
     * in the class file in ct.sym; in time, this information will be
     * available from the module system.
     */
    long getSupplementaryFlags(ClassSymbol c) {
        if (jrtIndex == null || !jrtIndex.isInJRT(c.classfile)) {
            return 0;
        }

        if (supplementaryFlags == null) {
            supplementaryFlags = new HashMap<>();
        }

        Long flags = supplementaryFlags.get(c.packge());
        if (flags == null) {
            long newFlags = 0;
            try {
                JRTIndex.CtSym ctSym = jrtIndex.getCtSym(c.packge().flatName());
                Profile minProfile = Profile.DEFAULT;
                if (ctSym.proprietary)
                    newFlags |= PROPRIETARY;
                if (ctSym.minProfile != null)
                    minProfile = Profile.lookup(ctSym.minProfile);
                if (profile != Profile.DEFAULT && minProfile.value > profile.value) {
                    newFlags |= NOT_IN_PROFILE;
                }
            } catch (IOException ignore) {
            }
            supplementaryFlags.put(c.packge(), flags = newFlags);
        }
        return flags;
    }

    private Map<PackageSymbol, Long> supplementaryFlags;

/************************************************************************
 * Loading Classes
 ***********************************************************************/

    /** Completion for classes to be loaded. Before a class is loaded
     *  we make sure its enclosing class (if any) is loaded.
     */
    private void complete(Symbol sym) throws CompletionFailure {
        if (sym.kind == TYP) {
            try {
                ClassSymbol c = (ClassSymbol) sym;
                dependencies.push(c, CompletionCause.CLASS_READER);
                annotate.blockAnnotations();
                c.members_field = new Scope.ErrorScope(c); // make sure it's always defined
                completeOwners(c.owner);
                completeEnclosing(c);
                fillIn(c);
            } finally {
                annotate.unblockAnnotationsNoFlush();
                dependencies.pop();
            }
        } else if (sym.kind == PCK) {
            PackageSymbol p = (PackageSymbol)sym;
            try {
                fillIn(p);
            } catch (IOException ex) {
                throw new CompletionFailure(sym, ex.getLocalizedMessage()).initCause(ex);
            }
        }
        if (!reader.filling)
            annotate.flush(); // finish attaching annotations
    }

    /** complete up through the enclosing package. */
    private void completeOwners(Symbol o) {
        if (o.kind != PCK) completeOwners(o.owner);
        o.complete();
    }

    /**
     * Tries to complete lexically enclosing classes if c looks like a
     * nested class.  This is similar to completeOwners but handles
     * the situation when a nested class is accessed directly as it is
     * possible with the Tree API or javax.lang.model.*.
     */
    private void completeEnclosing(ClassSymbol c) {
        if (c.owner.kind == PCK) {
            Symbol owner = c.owner;
            for (Name name : Convert.enclosingCandidates(Convert.shortName(c.name))) {
                Symbol encl = owner.members().findFirst(name);
                if (encl == null)
                    encl = syms.classes.get(TypeSymbol.formFlatName(name, owner));
                if (encl != null)
                    encl.complete();
            }
        }
    }

    /** Fill in definition of class `c' from corresponding class or
     *  source file.
     */
    private void fillIn(ClassSymbol c) {
        if (completionFailureName == c.fullname) {
            throw new CompletionFailure(c, "user-selected completion failure by class name");
        }
        currentOwner = c;
        JavaFileObject classfile = c.classfile;
        if (classfile != null) {
            JavaFileObject previousClassFile = currentClassFile;
            try {
                if (reader.filling) {
                    Assert.error("Filling " + classfile.toUri() + " during " + previousClassFile);
                }
                currentClassFile = classfile;
                if (verbose) {
                    log.printVerbose("loading", currentClassFile.toString());
                }
                if (classfile.getKind() == JavaFileObject.Kind.CLASS ||
                    classfile.getKind() == JavaFileObject.Kind.OTHER) {
                    reader.readClassFile(c);
                    c.flags_field |= getSupplementaryFlags(c);
                } else {
                    if (!sourceCompleter.isTerminal()) {
                        sourceCompleter.complete(c);
                    } else {
                        throw new IllegalStateException("Source completer required to read "
                                                        + classfile.toUri());
                    }
                }
            } finally {
                currentClassFile = previousClassFile;
            }
        } else {
            throw classFileNotFound(c);
        }
    }
    // where
        private CompletionFailure classFileNotFound(ClassSymbol c) {
            JCDiagnostic diag =
                diagFactory.fragment("class.file.not.found", c.flatname);
            return newCompletionFailure(c, diag);
        }
        /** Static factory for CompletionFailure objects.
         *  In practice, only one can be used at a time, so we share one
         *  to reduce the expense of allocating new exception objects.
         */
        private CompletionFailure newCompletionFailure(TypeSymbol c,
                                                       JCDiagnostic diag) {
            if (!cacheCompletionFailure) {
                // log.warning("proc.messager",
                //             Log.getLocalizedString("class.file.not.found", c.flatname));
                // c.debug.printStackTrace();
                return new CompletionFailure(c, diag);
            } else {
                CompletionFailure result = cachedCompletionFailure;
                result.sym = c;
                result.diag = diag;
                return result;
            }
        }
        private final CompletionFailure cachedCompletionFailure =
            new CompletionFailure(null, (JCDiagnostic) null);
        {
            cachedCompletionFailure.setStackTrace(new StackTraceElement[0]);
        }


    /** Load a toplevel class with given fully qualified name
     *  The class is entered into `classes' only if load was successful.
     */
    public ClassSymbol loadClass(Name flatname) throws CompletionFailure {
        boolean absent = syms.classes.get(flatname) == null;
        ClassSymbol c = syms.enterClass(flatname);
        if (c.members_field == null) {
            try {
                c.complete();
            } catch (CompletionFailure ex) {
                if (absent) syms.classes.remove(flatname);
                throw ex;
            }
        }
        return c;
    }

/************************************************************************
 * Loading Packages
 ***********************************************************************/

    /** Include class corresponding to given class file in package,
     *  unless (1) we already have one the same kind (.class or .java), or
     *         (2) we have one of the other kind, and the given class file
     *             is older.
     */
    protected void includeClassFile(PackageSymbol p, JavaFileObject file) {
        if ((p.flags_field & EXISTS) == 0)
            for (Symbol q = p; q != null && q.kind == PCK; q = q.owner)
                q.flags_field |= EXISTS;
        JavaFileObject.Kind kind = file.getKind();
        int seen;
        if (kind == JavaFileObject.Kind.CLASS || kind == JavaFileObject.Kind.OTHER)
            seen = CLASS_SEEN;
        else
            seen = SOURCE_SEEN;
        String binaryName = fileManager.inferBinaryName(currentLoc, file);
        int lastDot = binaryName.lastIndexOf(".");
        Name classname = names.fromString(binaryName.substring(lastDot + 1));
        boolean isPkgInfo = classname == names.package_info;
        ClassSymbol c = isPkgInfo
            ? p.package_info
            : (ClassSymbol) p.members_field.findFirst(classname);
        if (c == null) {
            c = syms.enterClass(classname, p);
            if (c.classfile == null) // only update the file if's it's newly created
                c.classfile = file;
            if (isPkgInfo) {
                p.package_info = c;
            } else {
                if (c.owner == p)  // it might be an inner class
                    p.members_field.enter(c);
            }
        } else if (!preferCurrent && c.classfile != null && (c.flags_field & seen) == 0) {
            // if c.classfile == null, we are currently compiling this class
            // and no further action is necessary.
            // if (c.flags_field & seen) != 0, we have already encountered
            // a file of the same kind; again no further action is necessary.
            if ((c.flags_field & (CLASS_SEEN | SOURCE_SEEN)) != 0)
                c.classfile = preferredFileObject(file, c.classfile);
        }
        c.flags_field |= seen;
    }

    /** Implement policy to choose to derive information from a source
     *  file or a class file when both are present.  May be overridden
     *  by subclasses.
     */
    protected JavaFileObject preferredFileObject(JavaFileObject a,
                                           JavaFileObject b) {

        if (preferSource)
            return (a.getKind() == JavaFileObject.Kind.SOURCE) ? a : b;
        else {
            long adate = a.getLastModified();
            long bdate = b.getLastModified();
            // 6449326: policy for bad lastModifiedTime in ClassReader
            //assert adate >= 0 && bdate >= 0;
            return (adate > bdate) ? a : b;
        }
    }

    /**
     * specifies types of files to be read when filling in a package symbol
     */
    protected EnumSet<JavaFileObject.Kind> getPackageFileKinds() {
        return EnumSet.of(JavaFileObject.Kind.CLASS, JavaFileObject.Kind.SOURCE);
    }

    /**
     * this is used to support javadoc
     */
    protected void extraFileActions(PackageSymbol pack, JavaFileObject fe) {
    }

    protected Location currentLoc; // FIXME

    private boolean verbosePath = true;

    // Set to true when the currently selected file should be kept
    private boolean preferCurrent;

    /** Load directory of package into members scope.
     */
    private void fillIn(PackageSymbol p) throws IOException {
        if (p.members_field == null)
            p.members_field = WriteableScope.create(p);

        preferCurrent = false;
        if (userPathsFirst) {
            scanUserPaths(p);
            preferCurrent = true;
            scanPlatformPath(p);
        } else {
            scanPlatformPath(p);
            scanUserPaths(p);
        }
        verbosePath = false;
    }

    /**
     * Scans class path and source path for files in given package.
     */
    private void scanUserPaths(PackageSymbol p) throws IOException {
        Set<JavaFileObject.Kind> kinds = getPackageFileKinds();

        Set<JavaFileObject.Kind> classKinds = EnumSet.copyOf(kinds);
        classKinds.remove(JavaFileObject.Kind.SOURCE);
        boolean wantClassFiles = !classKinds.isEmpty();

        Set<JavaFileObject.Kind> sourceKinds = EnumSet.copyOf(kinds);
        sourceKinds.remove(JavaFileObject.Kind.CLASS);
        boolean wantSourceFiles = !sourceKinds.isEmpty();

        boolean haveSourcePath = fileManager.hasLocation(SOURCE_PATH);

        if (verbose && verbosePath) {
            if (fileManager instanceof StandardJavaFileManager) {
                StandardJavaFileManager fm = (StandardJavaFileManager)fileManager;
                if (haveSourcePath && wantSourceFiles) {
                    List<File> path = List.nil();
                    for (File file : fm.getLocation(SOURCE_PATH)) {
                        path = path.prepend(file);
                    }
                    log.printVerbose("sourcepath", path.reverse().toString());
                } else if (wantSourceFiles) {
                    List<File> path = List.nil();
                    for (File file : fm.getLocation(CLASS_PATH)) {
                        path = path.prepend(file);
                    }
                    log.printVerbose("sourcepath", path.reverse().toString());
                }
                if (wantClassFiles) {
                    List<File> path = List.nil();
                    for (File file : fm.getLocation(PLATFORM_CLASS_PATH)) {
                        path = path.prepend(file);
                    }
                    for (File file : fm.getLocation(CLASS_PATH)) {
                        path = path.prepend(file);
                    }
                    log.printVerbose("classpath",  path.reverse().toString());
                }
            }
        }

        String packageName = p.fullname.toString();
        if (wantSourceFiles && !haveSourcePath) {
            fillIn(p, CLASS_PATH,
                   fileManager.list(CLASS_PATH,
                                    packageName,
                                    kinds,
                                    false));
        } else {
            if (wantClassFiles)
                fillIn(p, CLASS_PATH,
                       fileManager.list(CLASS_PATH,
                                        packageName,
                                        classKinds,
                                        false));
            if (wantSourceFiles)
                fillIn(p, SOURCE_PATH,
                       fileManager.list(SOURCE_PATH,
                                        packageName,
                                        sourceKinds,
                                        false));
        }
    }

    /**
     * Scans platform class path for files in given package.
     */
    private void scanPlatformPath(PackageSymbol p) throws IOException {
        fillIn(p, PLATFORM_CLASS_PATH,
               fileManager.list(PLATFORM_CLASS_PATH,
                                p.fullname.toString(),
                                allowSigFiles ? EnumSet.of(JavaFileObject.Kind.CLASS,
                                                           JavaFileObject.Kind.OTHER)
                                              : EnumSet.of(JavaFileObject.Kind.CLASS),
                                false));
    }
    // where
        @SuppressWarnings("fallthrough")
        private void fillIn(PackageSymbol p,
                            Location location,
                            Iterable<JavaFileObject> files)
        {
            currentLoc = location;
            for (JavaFileObject fo : files) {
                switch (fo.getKind()) {
                case OTHER:
                    boolean sigFile = location == PLATFORM_CLASS_PATH &&
                                      allowSigFiles &&
                                      fo.getName().endsWith(".sig");
                    if (!sigFile) {
                        extraFileActions(p, fo);
                        break;
                    }
                    //intentional fall-through:
                case CLASS:
                case SOURCE: {
                    // TODO pass binaryName to includeClassFile
                    String binaryName = fileManager.inferBinaryName(currentLoc, fo);
                    String simpleName = binaryName.substring(binaryName.lastIndexOf(".") + 1);
                    if (SourceVersion.isIdentifier(simpleName) ||
                        simpleName.equals("package-info"))
                        includeClassFile(p, fo);
                    break;
                }
                default:
                    extraFileActions(p, fo);
                }
            }
        }

    /**
     * Used for bad class definition files, such as bad .class files or
     * for .java files with unexpected package or class names.
     */
    public static class BadClassFile extends CompletionFailure {
        private static final long serialVersionUID = 0;

        public BadClassFile(TypeSymbol sym, JavaFileObject file, JCDiagnostic diag,
                JCDiagnostic.Factory diagFactory) {
            super(sym, createBadClassFileDiagnostic(file, diag, diagFactory));
        }
        // where
        private static JCDiagnostic createBadClassFileDiagnostic(
                JavaFileObject file, JCDiagnostic diag, JCDiagnostic.Factory diagFactory) {
            String key = (file.getKind() == JavaFileObject.Kind.SOURCE
                        ? "bad.source.file.header" : "bad.class.file.header");
            return diagFactory.fragment(key, file, diag);
        }
    }
}
