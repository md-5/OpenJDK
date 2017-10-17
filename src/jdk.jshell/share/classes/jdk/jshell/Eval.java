/*
 * Copyright (c) 2014, 2016, Oracle and/or its affiliates. All rights reserved.
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
package jdk.jshell;

import java.util.ArrayList;
import java.util.Collection;
import java.util.Collections;
import java.util.List;
import java.util.Locale;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import java.util.stream.Collectors;
import javax.lang.model.element.Modifier;
import com.sun.source.tree.ArrayTypeTree;
import com.sun.source.tree.AssignmentTree;
import com.sun.source.tree.ClassTree;
import com.sun.source.tree.ExpressionTree;
import com.sun.source.tree.IdentifierTree;
import com.sun.source.tree.MethodTree;
import com.sun.source.tree.ModifiersTree;
import com.sun.source.tree.NewClassTree;
import com.sun.source.tree.Tree;
import com.sun.source.tree.VariableTree;
import com.sun.tools.javac.tree.JCTree;
import com.sun.tools.javac.tree.Pretty;
import java.io.IOException;
import java.io.StringWriter;
import java.io.Writer;
import java.util.LinkedHashSet;
import java.util.Set;
import jdk.jshell.ExpressionToTypeInfo.ExpressionInfo;
import jdk.jshell.Key.ErroneousKey;
import jdk.jshell.Key.MethodKey;
import jdk.jshell.Key.TypeDeclKey;
import jdk.jshell.Snippet.Kind;
import jdk.jshell.Snippet.SubKind;
import jdk.jshell.TaskFactory.AnalyzeTask;
import jdk.jshell.TaskFactory.BaseTask;
import jdk.jshell.TaskFactory.CompileTask;
import jdk.jshell.TaskFactory.ParseTask;
import jdk.jshell.Wrap.CompoundWrap;
import jdk.jshell.Wrap.Range;
import jdk.jshell.Snippet.Status;
import jdk.jshell.spi.ExecutionControl.ClassBytecodes;
import jdk.jshell.spi.ExecutionControl.ClassInstallException;
import jdk.jshell.spi.ExecutionControl.EngineTerminationException;
import jdk.jshell.spi.ExecutionControl.InternalException;
import jdk.jshell.spi.ExecutionControl.NotImplementedException;
import jdk.jshell.spi.ExecutionControl.ResolutionException;
import jdk.jshell.spi.ExecutionControl.RunException;
import jdk.jshell.spi.ExecutionControl.UserException;
import static java.util.stream.Collectors.toList;
import static java.util.stream.Collectors.toSet;
import static java.util.Collections.singletonList;
import static jdk.internal.jshell.debug.InternalDebugControl.DBG_GEN;
import static jdk.jshell.Util.DOIT_METHOD_NAME;
import static jdk.jshell.Util.PREFIX_PATTERN;
import static jdk.jshell.Util.expunge;
import static jdk.jshell.Snippet.SubKind.SINGLE_TYPE_IMPORT_SUBKIND;
import static jdk.jshell.Snippet.SubKind.SINGLE_STATIC_IMPORT_SUBKIND;
import static jdk.jshell.Snippet.SubKind.TYPE_IMPORT_ON_DEMAND_SUBKIND;
import static jdk.jshell.Snippet.SubKind.STATIC_IMPORT_ON_DEMAND_SUBKIND;

/**
 * The Evaluation Engine. Source internal analysis, wrapping control,
 * compilation, declaration. redefinition, replacement, and execution.
 *
 * @author Robert Field
 */
class Eval {

    private static final Pattern IMPORT_PATTERN = Pattern.compile("import\\p{javaWhitespace}+(?<static>static\\p{javaWhitespace}+)?(?<fullname>[\\p{L}\\p{N}_\\$\\.]+\\.(?<name>[\\p{L}\\p{N}_\\$]+|\\*))");

    // for uses that should not change state -- non-evaluations
    private boolean preserveState = false;

    private int varNumber = 0;

    private final JShell state;

    Eval(JShell state) {
        this.state = state;
    }

    /**
     * Evaluates a snippet of source.
     *
     * @param userSource the source of the snippet
     * @return the list of primary and update events
     * @throws IllegalStateException
     */
    List<SnippetEvent> eval(String userSource) throws IllegalStateException {
        List<SnippetEvent> allEvents = new ArrayList<>();
        for (Snippet snip : sourceToSnippets(userSource)) {
            if (snip.kind() == Kind.ERRONEOUS) {
                state.maps.installSnippet(snip);
                allEvents.add(new SnippetEvent(
                        snip, Status.NONEXISTENT, Status.REJECTED,
                        false, null, null, null));
            } else {
                allEvents.addAll(declare(snip, snip.syntheticDiags()));
            }
        }
        return allEvents;
    }

    /**
     * Converts the user source of a snippet into a Snippet list -- Snippet will
     * have wrappers.
     *
     * @param userSource the source of the snippet
     * @return usually a singleton list of Snippet, but may be empty or multiple
     */
    List<Snippet> sourceToSnippetsWithWrappers(String userSource) {
        List<Snippet> snippets = sourceToSnippets(userSource);
        for (Snippet snip : snippets) {
            if (snip.outerWrap() == null) {
                snip.setOuterWrap(
                        (snip.kind() == Kind.IMPORT)
                                ? state.outerMap.wrapImport(snip.guts(), snip)
                                : state.outerMap.wrapInTrialClass(snip.guts())
                );
            }
        }
        return snippets;
    }

    /**
     * Converts the user source of a snippet into a Snippet object (or list of
     * objects in the case of: int x, y, z;).  Does not install the Snippets
     * or execute them.  Does not change any state.
     *
     * @param userSource the source of the snippet
     * @return usually a singleton list of Snippet, but may be empty or multiple
     */
    List<Snippet> toScratchSnippets(String userSource) {
        try {
            preserveState = true;
            return sourceToSnippets(userSource);
        } finally {
            preserveState = false;
        }
    }

    /**
     * Converts the user source of a snippet into a Snippet object (or list of
     * objects in the case of: int x, y, z;).  Does not install the Snippets
     * or execute them.
     *
     * @param userSource the source of the snippet
     * @return usually a singleton list of Snippet, but may be empty or multiple
     */
    private List<Snippet> sourceToSnippets(String userSource) {
        String compileSource = Util.trimEnd(new MaskCommentsAndModifiers(userSource, false).cleared());
        if (compileSource.length() == 0) {
            return Collections.emptyList();
        }
        ParseTask pt = state.taskFactory.parse(compileSource);
        List<? extends Tree> units = pt.units();
        if (units.isEmpty()) {
            return compileFailResult(pt, userSource, Kind.ERRONEOUS);
        }
        Tree unitTree = units.get(0);
        if (pt.getDiagnostics().hasOtherThanNotStatementErrors()) {
            return compileFailResult(pt, userSource, kindOfTree(unitTree));
        }

        // Erase illegal/ignored modifiers
        compileSource = new MaskCommentsAndModifiers(compileSource, true).cleared();

        state.debug(DBG_GEN, "Kind: %s -- %s\n", unitTree.getKind(), unitTree);
        switch (unitTree.getKind()) {
            case IMPORT:
                return processImport(userSource, compileSource);
            case VARIABLE:
                return processVariables(userSource, units, compileSource, pt);
            case EXPRESSION_STATEMENT:
                return processExpression(userSource, compileSource);
            case CLASS:
                return processClass(userSource, unitTree, compileSource, SubKind.CLASS_SUBKIND, pt);
            case ENUM:
                return processClass(userSource, unitTree, compileSource, SubKind.ENUM_SUBKIND, pt);
            case ANNOTATION_TYPE:
                return processClass(userSource, unitTree, compileSource, SubKind.ANNOTATION_TYPE_SUBKIND, pt);
            case INTERFACE:
                return processClass(userSource, unitTree, compileSource, SubKind.INTERFACE_SUBKIND, pt);
            case METHOD:
                return processMethod(userSource, unitTree, compileSource, pt);
            default:
                return processStatement(userSource, compileSource);
        }
    }

    private List<Snippet> processImport(String userSource, String compileSource) {
        Wrap guts = Wrap.simpleWrap(compileSource);
        Matcher mat = IMPORT_PATTERN.matcher(compileSource);
        String fullname;
        String name;
        boolean isStatic;
        if (mat.find()) {
            isStatic = mat.group("static") != null;
            name = mat.group("name");
            fullname = mat.group("fullname");
        } else {
            // bad import -- fake it
            isStatic = compileSource.contains("static");
            name = fullname = compileSource;
        }
        String fullkey = (isStatic ? "static-" : "") + fullname;
        boolean isStar = name.equals("*");
        String keyName = isStar
                ? fullname
                : name;
        SubKind snippetKind = isStar
                ? (isStatic ? STATIC_IMPORT_ON_DEMAND_SUBKIND : TYPE_IMPORT_ON_DEMAND_SUBKIND)
                : (isStatic ? SINGLE_STATIC_IMPORT_SUBKIND : SINGLE_TYPE_IMPORT_SUBKIND);
        Snippet snip = new ImportSnippet(state.keyMap.keyForImport(keyName, snippetKind),
                userSource, guts, fullname, name, snippetKind, fullkey, isStatic, isStar);
        return singletonList(snip);
    }

    private static class EvalPretty extends Pretty {

        private final Writer out;

        public EvalPretty(Writer writer, boolean bln) {
            super(writer, bln);
            this.out = writer;
        }

        /**
         * Print string, DO NOT replacing all non-ascii character with unicode
         * escapes.
         */
        @Override
        public void print(Object o) throws IOException {
            out.write(o.toString());
        }

        static String prettyExpr(JCTree tree, boolean bln) {
            StringWriter out = new StringWriter();
            try {
                new EvalPretty(out, bln).printExpr(tree);
            } catch (IOException e) {
                throw new AssertionError(e);
            }
            return out.toString();
        }
    }

    private List<Snippet> processVariables(String userSource, List<? extends Tree> units, String compileSource, ParseTask pt) {
        List<Snippet> snippets = new ArrayList<>();
        TreeDissector dis = TreeDissector.createByFirstClass(pt);
        for (Tree unitTree : units) {
            VariableTree vt = (VariableTree) unitTree;
            String name = vt.getName().toString();
            String typeName;
            String fullTypeName;
            TreeDependencyScanner tds = new TreeDependencyScanner();
            Wrap typeWrap;
            Wrap anonDeclareWrap = null;
            Wrap winit = null;
            StringBuilder sbBrackets = new StringBuilder();
            Tree baseType = vt.getType();
            if (baseType != null) {
                tds.scan(baseType); // Not dependent on initializer
                fullTypeName = typeName = EvalPretty.prettyExpr((JCTree) vt.getType(), false);
                while (baseType instanceof ArrayTypeTree) {
                    //TODO handle annotations too
                    baseType = ((ArrayTypeTree) baseType).getType();
                    sbBrackets.append("[]");
                }
                Range rtype = dis.treeToRange(baseType);
                typeWrap = Wrap.rangeWrap(compileSource, rtype);
            } else {
                AnalyzeTask at = trialCompile(Wrap.methodWrap(compileSource));
                if (at.hasErrors()) {
                    return compileFailResult(at, userSource, kindOfTree(unitTree));
                }
                Tree init = vt.getInitializer();
                if (init != null) {
                    Range rinit = dis.treeToRange(init);
                    String initCode = rinit.part(compileSource);
                    ExpressionInfo ei =
                            ExpressionToTypeInfo.localVariableTypeForInitializer(initCode, state);
                    typeName = ei == null ? "java.lang.Object" : ei.typeName;
                    fullTypeName = ei == null ? "java.lang.Object" : ei.fullTypeName;
                    if (ei != null && init.getKind() == Tree.Kind.NEW_CLASS &&
                        ((NewClassTree) init).getClassBody() != null) {
                        NewClassTree nct = (NewClassTree) init;
                        StringBuilder constructor = new StringBuilder();
                        constructor.append(fullTypeName).append("(");
                        String sep = "";
                        if (ei.enclosingInstanceType != null) {
                            constructor.append(ei.enclosingInstanceType);
                            constructor.append(" encl");
                            sep = ", ";
                        }
                        int idx = 0;
                        for (String type : ei.parameterTypes) {
                            constructor.append(sep);
                            constructor.append(type);
                            constructor.append(" ");
                            constructor.append("arg" + idx++);
                            sep = ", ";
                        }
                        if (ei.enclosingInstanceType != null) {
                            constructor.append(") { encl.super (");
                        } else {
                            constructor.append(") { super (");
                        }
                        sep = "";
                        for (int i = 0; i < idx; i++) {
                            constructor.append(sep);
                            constructor.append("arg" + i++);
                            sep = ", ";
                        }
                        constructor.append("); }");
                        List<? extends Tree> members = nct.getClassBody().getMembers();
                        Range bodyRange = dis.treeListToRange(members);
                        Wrap bodyWrap;

                        if (bodyRange != null) {
                            bodyWrap = Wrap.rangeWrap(compileSource, bodyRange);
                        } else {
                            bodyWrap = Wrap.simpleWrap(" ");
                        }

                        Range argRange = dis.treeListToRange(nct.getArguments());
                        Wrap argWrap;

                        if (argRange != null) {
                            argWrap = Wrap.rangeWrap(compileSource, argRange);
                        } else {
                            argWrap = Wrap.simpleWrap(" ");
                        }

                        if (ei.enclosingInstanceType != null) {
                            Range enclosingRanges =
                                    dis.treeToRange(nct.getEnclosingExpression());
                            Wrap enclosingWrap = Wrap.rangeWrap(compileSource, enclosingRanges);
                            argWrap = argRange != null ? new CompoundWrap(enclosingWrap,
                                                                          Wrap.simpleWrap(","),
                                                                          argWrap)
                                                       : enclosingWrap;
                        }
                        Wrap hwrap = Wrap.simpleWrap("public static class " + fullTypeName +
                                                     (ei.isClass ? " extends " : " implements ") +
                                                     typeName + " { " + constructor);
                        anonDeclareWrap = new CompoundWrap(hwrap, bodyWrap, Wrap.simpleWrap("}"));
                        winit = new CompoundWrap("new " + fullTypeName + "(", argWrap, ")");

                        String superType = typeName;

                        typeName = fullTypeName;
                        fullTypeName = ei.isClass ? "<anonymous class extending " + superType + ">"
                                                  : "<anonymous class implementing " + superType + ">";
                    }
                    tds.scan(init);
                } else {
                    fullTypeName = typeName = "java.lang.Object";
                }
                typeWrap = Wrap.identityWrap(typeName);
            }
            Range runit = dis.treeToRange(vt);
            runit = new Range(runit.begin, runit.end - 1);
            ExpressionTree it = vt.getInitializer();
            int nameMax = runit.end - 1;
            SubKind subkind;
            if (it != null) {
                subkind = SubKind.VAR_DECLARATION_WITH_INITIALIZER_SUBKIND;
                Range rinit = dis.treeToRange(it);
                winit = winit == null ? Wrap.rangeWrap(compileSource, rinit) : winit;
                nameMax = rinit.begin - 1;
            } else {
                subkind = SubKind.VAR_DECLARATION_SUBKIND;
            }
            int nameStart = compileSource.lastIndexOf(name, nameMax);
            if (nameStart < 0) {
                throw new AssertionError("Name '" + name + "' not found");
            }
            int nameEnd = nameStart + name.length();
            Range rname = new Range(nameStart, nameEnd);
            Wrap guts = Wrap.varWrap(compileSource, typeWrap, sbBrackets.toString(), rname,
                                     winit, anonDeclareWrap);
                        DiagList modDiag = modifierDiagnostics(vt.getModifiers(), dis, true);
            Snippet snip = new VarSnippet(state.keyMap.keyForVariable(name), userSource, guts,
                    name, subkind, fullTypeName,
                    tds.declareReferences(), modDiag);
            snippets.add(snip);
        }
        return snippets;
    }

    private List<Snippet> processExpression(String userSource, String compileSource) {
        String name = null;
        ExpressionInfo ei = ExpressionToTypeInfo.expressionInfo(compileSource, state);
        ExpressionTree assignVar;
        Wrap guts;
        Snippet snip;
        if (ei != null && ei.isNonVoid) {
            String typeName = ei.typeName;
            SubKind subkind;
            if (ei.tree instanceof IdentifierTree) {
                IdentifierTree id = (IdentifierTree) ei.tree;
                name = id.getName().toString();
                subkind = SubKind.VAR_VALUE_SUBKIND;

            } else if (ei.tree instanceof AssignmentTree
                    && (assignVar = ((AssignmentTree) ei.tree).getVariable()) instanceof IdentifierTree) {
                name = assignVar.toString();
                subkind = SubKind.ASSIGNMENT_SUBKIND;
            } else {
                subkind = SubKind.OTHER_EXPRESSION_SUBKIND;
            }
            if (shouldGenTempVar(subkind)) {
                if (preserveState) {
                    name = "$$";
                } else {
                    if (state.tempVariableNameGenerator != null) {
                        name = state.tempVariableNameGenerator.get();
                    }
                    while (name == null || state.keyMap.doesVariableNameExist(name)) {
                        name = "$" + ++varNumber;
                    }
                }
                guts = Wrap.tempVarWrap(compileSource, typeName, name);
                Collection<String> declareReferences = null; //TODO
                snip = new VarSnippet(state.keyMap.keyForVariable(name), userSource, guts,
                        name, SubKind.TEMP_VAR_EXPRESSION_SUBKIND, typeName, declareReferences, null);
            } else {
                guts = Wrap.methodReturnWrap(compileSource);
                snip = new ExpressionSnippet(state.keyMap.keyForExpression(name, typeName), userSource, guts,
                        name, subkind);
            }
        } else {
            guts = Wrap.methodWrap(compileSource);
            if (ei == null) {
                // We got no type info, check for not a statement by trying
                AnalyzeTask at = trialCompile(guts);
                if (at.getDiagnostics().hasNotStatement()) {
                    guts = Wrap.methodReturnWrap(compileSource);
                    at = trialCompile(guts);
                }
                if (at.hasErrors()) {
                    return compileFailResult(at, userSource, Kind.EXPRESSION);
                }
            }
            snip = new StatementSnippet(state.keyMap.keyForStatement(), userSource, guts);
        }
        return singletonList(snip);
    }

    private List<Snippet> processClass(String userSource, Tree unitTree, String compileSource, SubKind snippetKind, ParseTask pt) {
        TreeDependencyScanner tds = new TreeDependencyScanner();
        tds.scan(unitTree);

        TreeDissector dis = TreeDissector.createByFirstClass(pt);

        ClassTree klassTree = (ClassTree) unitTree;
        String name = klassTree.getSimpleName().toString();
        DiagList modDiag = modifierDiagnostics(klassTree.getModifiers(), dis, false);
        TypeDeclKey key = state.keyMap.keyForClass(name);
        // Corralling mutates.  Must be last use of pt, unitTree, klassTree
        Wrap corralled = new Corraller(key.index(), pt.getContext()).corralType(klassTree);

        Wrap guts = Wrap.classMemberWrap(compileSource);
        Snippet snip = new TypeDeclSnippet(key, userSource, guts,
                name, snippetKind,
                corralled, tds.declareReferences(), tds.bodyReferences(), modDiag);
        return singletonList(snip);
    }

    private List<Snippet> processStatement(String userSource, String compileSource) {
        Wrap guts = Wrap.methodWrap(compileSource);
        // Check for unreachable by trying
        AnalyzeTask at = trialCompile(guts);
        if (at.hasErrors()) {
            if (at.getDiagnostics().hasUnreachableError()) {
                guts = Wrap.methodUnreachableSemiWrap(compileSource);
                at = trialCompile(guts);
                if (at.hasErrors()) {
                    if (at.getDiagnostics().hasUnreachableError()) {
                        // Without ending semicolon
                        guts = Wrap.methodUnreachableWrap(compileSource);
                        at = trialCompile(guts);
                    }
                    if (at.hasErrors()) {
                        return compileFailResult(at, userSource, Kind.STATEMENT);
                    }
                }
            } else {
                return compileFailResult(at, userSource, Kind.STATEMENT);
            }
        }
        Snippet snip = new StatementSnippet(state.keyMap.keyForStatement(), userSource, guts);
        return singletonList(snip);
    }

    private AnalyzeTask trialCompile(Wrap guts) {
        OuterWrap outer = state.outerMap.wrapInTrialClass(guts);
        return state.taskFactory.new AnalyzeTask(outer);
    }

    private List<Snippet> processMethod(String userSource, Tree unitTree, String compileSource, ParseTask pt) {
        TreeDependencyScanner tds = new TreeDependencyScanner();
        tds.scan(unitTree);
        TreeDissector dis = TreeDissector.createByFirstClass(pt);

        MethodTree mt = (MethodTree) unitTree;
        String name = mt.getName().toString();
        String parameterTypes
                = mt.getParameters()
                .stream()
                .map(param -> dis.treeToRange(param.getType()).part(compileSource))
                .collect(Collectors.joining(","));
        Tree returnType = mt.getReturnType();
        DiagList modDiag = modifierDiagnostics(mt.getModifiers(), dis, true);
        MethodKey key = state.keyMap.keyForMethod(name, parameterTypes);
        // Corralling mutates.  Must be last use of pt, unitTree, mt
        Wrap corralled = new Corraller(key.index(), pt.getContext()).corralMethod(mt);

        if (modDiag.hasErrors()) {
            return compileFailResult(modDiag, userSource, Kind.METHOD);
        }
        Wrap guts = Wrap.classMemberWrap(compileSource);
        Range typeRange = dis.treeToRange(returnType);
        String signature = "(" + parameterTypes + ")" + typeRange.part(compileSource);

        Snippet snip = new MethodSnippet(key, userSource, guts,
                name, signature,
                corralled, tds.declareReferences(), tds.bodyReferences(), modDiag);
        return singletonList(snip);
    }

    private Kind kindOfTree(Tree tree) {
        switch (tree.getKind()) {
            case IMPORT:
                return Kind.IMPORT;
            case VARIABLE:
                return Kind.VAR;
            case EXPRESSION_STATEMENT:
                return Kind.EXPRESSION;
            case CLASS:
            case ENUM:
            case ANNOTATION_TYPE:
            case INTERFACE:
                return Kind.TYPE_DECL;
            case METHOD:
                return Kind.METHOD;
            default:
                return Kind.STATEMENT;
        }
    }

    /**
     * The snippet has failed, return with the rejected snippet
     *
     * @param xt the task from which to extract the failure diagnostics
     * @param userSource the incoming bad user source
     * @return a rejected snippet
     */
    private List<Snippet> compileFailResult(BaseTask xt, String userSource, Kind probableKind) {
        return compileFailResult(xt.getDiagnostics(), userSource, probableKind);
    }

    /**
     * The snippet has failed, return with the rejected snippet
     *
     * @param diags the failure diagnostics
     * @param userSource the incoming bad user source
     * @return a rejected snippet
     */
    private List<Snippet> compileFailResult(DiagList diags, String userSource, Kind probableKind) {
        ErroneousKey key = state.keyMap.keyForErroneous();
        Snippet snip = new ErroneousSnippet(key, userSource, null,
                probableKind, SubKind.UNKNOWN_SUBKIND);
        snip.setFailed(diags);

        // Install  wrapper for query by SourceCodeAnalysis.wrapper
        String compileSource = Util.trimEnd(new MaskCommentsAndModifiers(userSource, true).cleared());
        OuterWrap outer;
        switch (probableKind) {
            case IMPORT:
                outer = state.outerMap.wrapImport(Wrap.simpleWrap(compileSource), snip);
                break;
            case EXPRESSION:
                outer = state.outerMap.wrapInTrialClass(Wrap.methodReturnWrap(compileSource));
                break;
            case VAR:
            case TYPE_DECL:
            case METHOD:
                outer = state.outerMap.wrapInTrialClass(Wrap.classMemberWrap(compileSource));
                break;
            default:
                outer = state.outerMap.wrapInTrialClass(Wrap.methodWrap(compileSource));
                break;
        }
        snip.setOuterWrap(outer);

        return singletonList(snip);
    }

    /**
     * Should a temp var wrap the expression. TODO make this user configurable.
     *
     * @param snippetKind
     * @return
     */
    private boolean shouldGenTempVar(SubKind snippetKind) {
        return snippetKind == SubKind.OTHER_EXPRESSION_SUBKIND;
    }

    List<SnippetEvent> drop(Snippet si) {
        Unit c = new Unit(state, si);
        Set<Unit> outs;
        if (si instanceof PersistentSnippet) {
            Set<Unit> ins = c.dependents().collect(toSet());
            outs = compileAndLoad(ins);
        } else {
            outs = Collections.emptySet();
        }
        return events(c, outs, null, null);
    }

    private List<SnippetEvent> declare(Snippet si, DiagList generatedDiagnostics) {
        Unit c = new Unit(state, si, null, generatedDiagnostics);
        Set<Unit> ins = new LinkedHashSet<>();
        ins.add(c);
        Set<Unit> outs = compileAndLoad(ins);

        if (!si.status().isDefined()
                && si.diagnostics().isEmpty()
                && si.unresolved().isEmpty()) {
            // did not succeed, but no record of it, extract from others
            si.setDiagnostics(outs.stream()
                    .flatMap(u -> u.snippet().diagnostics().stream())
                    .collect(Collectors.toCollection(DiagList::new)));
        }

        // If appropriate, execute the snippet
        String value = null;
        JShellException exception = null;
        if (si.status().isDefined()) {
            if (si.isExecutable()) {
                try {
                    value = state.executionControl().invoke(si.classFullName(), DOIT_METHOD_NAME);
                    value = si.subKind().hasValue()
                            ? expunge(value)
                            : "";
                } catch (ResolutionException ex) {
                    DeclarationSnippet sn = (DeclarationSnippet) state.maps.getSnippetDeadOrAlive(ex.id());
                    exception = new UnresolvedReferenceException(sn, translateExceptionStack(ex));
                } catch (UserException ex) {
                    exception = new EvalException(ex.getMessage(),
                            ex.causeExceptionClass(),
                            translateExceptionStack(ex));
                } catch (RunException ex) {
                    // StopException - no-op
                } catch (InternalException ex) {
                    state.debug(ex, "invoke");
                } catch (EngineTerminationException ex) {
                    state.closeDown();
                }
            } else if (si.subKind() == SubKind.VAR_DECLARATION_SUBKIND) {
                switch (((VarSnippet) si).typeName()) {
                    case "byte":
                    case "short":
                    case "int":
                    case "long":
                        value = "0";
                        break;
                    case "float":
                    case "double":
                        value = "0.0";
                        break;
                    case "boolean":
                        value = "false";
                        break;
                    case "char":
                        value = "''";
                        break;
                    default:
                        value = "null";
                        break;
                }
            }
        }
        return events(c, outs, value, exception);
    }

    private boolean interestingEvent(SnippetEvent e) {
        return e.isSignatureChange()
                    || e.causeSnippet() == null
                    || e.status() != e.previousStatus()
                    || e.exception() != null;
    }

    private List<SnippetEvent> events(Unit c, Collection<Unit> outs, String value, JShellException exception) {
        List<SnippetEvent> events = new ArrayList<>();
        events.add(c.event(value, exception));
        events.addAll(outs.stream()
                .filter(u -> u != c)
                .map(u -> u.event(null, null))
                .filter(this::interestingEvent)
                .collect(Collectors.toList()));
        events.addAll(outs.stream()
                .flatMap(u -> u.secondaryEvents().stream())
                .filter(this::interestingEvent)
                .collect(Collectors.toList()));
        //System.err.printf("Events: %s\n", events);
        return events;
    }

    private Set<OuterWrap> outerWrapSet(Collection<Unit> units) {
        return units.stream()
                .map(u -> u.snippet().outerWrap())
                .collect(toSet());
    }

    private Set<Unit> compileAndLoad(Set<Unit> ins) {
        if (ins.isEmpty()) {
            return ins;
        }
        Set<Unit> replaced = new LinkedHashSet<>();
        // Loop until dependencies and errors are stable
        while (true) {
            state.debug(DBG_GEN, "compileAndLoad  %s\n", ins);

            ins.stream().forEach(Unit::initialize);
            ins.stream().forEach(u -> u.setWrap(ins, ins));
            AnalyzeTask at = state.taskFactory.new AnalyzeTask(outerWrapSet(ins));
            ins.stream().forEach(u -> u.setDiagnostics(at));

            // corral any Snippets that need it
            AnalyzeTask cat;
            if (ins.stream().anyMatch(u -> u.corralIfNeeded(ins))) {
                // if any were corralled, re-analyze everything
                cat = state.taskFactory.new AnalyzeTask(outerWrapSet(ins));
                ins.stream().forEach(u -> u.setCorralledDiagnostics(cat));
            } else {
                cat = at;
            }
            ins.stream().forEach(u -> u.setStatus(cat));
            // compile and load the legit snippets
            boolean success;
            while (true) {
                List<Unit> legit = ins.stream()
                        .filter(Unit::isDefined)
                        .collect(toList());
                state.debug(DBG_GEN, "compileAndLoad ins = %s -- legit = %s\n",
                        ins, legit);
                if (legit.isEmpty()) {
                    // no class files can be generated
                    success = true;
                } else {
                    // re-wrap with legit imports
                    legit.stream().forEach(u -> u.setWrap(ins, legit));

                    // generate class files for those capable
                    CompileTask ct = state.taskFactory.new CompileTask(outerWrapSet(legit));
                    if (!ct.compile()) {
                        // oy! compile failed because of recursive new unresolved
                        if (legit.stream()
                                .filter(u -> u.smashingErrorDiagnostics(ct))
                                .count() > 0) {
                            // try again, with the erroreous removed
                            continue;
                        } else {
                            state.debug(DBG_GEN, "Should never happen error-less failure - %s\n",
                                    legit);
                        }
                    }

                    // load all new classes
                    load(legit.stream()
                            .flatMap(u -> u.classesToLoad(ct.classList(u.snippet().outerWrap())))
                            .collect(toSet()));
                    // attempt to redefine the remaining classes
                    List<Unit> toReplace = legit.stream()
                            .filter(u -> !u.doRedefines())
                            .collect(toList());

                    // prevent alternating redefine/replace cyclic dependency
                    // loop by replacing all that have been replaced
                    if (!toReplace.isEmpty()) {
                        replaced.addAll(toReplace);
                        replaced.stream().forEach(Unit::markForReplacement);
                    }

                    success = toReplace.isEmpty();
                }
                break;
            }

            // add any new dependencies to the working set
            List<Unit> newDependencies = ins.stream()
                    .flatMap(Unit::effectedDependents)
                    .collect(toList());
            state.debug(DBG_GEN, "compileAndLoad %s -- deps: %s  success: %s\n",
                    ins, newDependencies, success);
            if (!ins.addAll(newDependencies) && success) {
                // all classes that could not be directly loaded (because they
                // are new) have been redefined, and no new dependnencies were
                // identified
                ins.stream().forEach(Unit::finish);
                return ins;
            }
        }
    }

    /**
     * If there are classes to load, loads by calling the execution engine.
     * @param classbytecodes names of the classes to load.
     */
    private void load(Collection<ClassBytecodes> classbytecodes) {
        if (!classbytecodes.isEmpty()) {
            ClassBytecodes[] cbcs = classbytecodes.toArray(new ClassBytecodes[classbytecodes.size()]);
            try {
                state.executionControl().load(cbcs);
                state.classTracker.markLoaded(cbcs);
            } catch (ClassInstallException ex) {
                state.classTracker.markLoaded(cbcs, ex.installed());
            } catch (NotImplementedException ex) {
                state.debug(ex, "Seriously?!? load not implemented");
                state.closeDown();
            } catch (EngineTerminationException ex) {
                state.closeDown();
            }
        }
    }

    private StackTraceElement[] translateExceptionStack(Exception ex) {
        StackTraceElement[] raw = ex.getStackTrace();
        int last = raw.length;
        do {
            if (last == 0) {
                last = raw.length - 1;
                break;
            }
        } while (!isWrap(raw[--last]));
        StackTraceElement[] elems = new StackTraceElement[last + 1];
        for (int i = 0; i <= last; ++i) {
            StackTraceElement r = raw[i];
            OuterSnippetsClassWrap outer = state.outerMap.getOuter(r.getClassName());
            if (outer != null) {
                String klass = expunge(r.getClassName());
                String method = r.getMethodName().equals(DOIT_METHOD_NAME) ? "" : r.getMethodName();
                int wln = r.getLineNumber() - 1;
                int line = outer.wrapLineToSnippetLine(wln) + 1;
                Snippet sn = outer.wrapLineToSnippet(wln);
                String file = "#" + sn.id();
                elems[i] = new StackTraceElement(klass, method, file, line);
            } else if (r.getFileName().equals("<none>")) {
                elems[i] = new StackTraceElement(r.getClassName(), r.getMethodName(), null, r.getLineNumber());
            } else {
                elems[i] = r;
            }
        }
        return elems;
    }

    private boolean isWrap(StackTraceElement ste) {
        return PREFIX_PATTERN.matcher(ste.getClassName()).find();
    }

    private DiagList modifierDiagnostics(ModifiersTree modtree,
            final TreeDissector dis, boolean isAbstractProhibited) {

        class ModifierDiagnostic extends Diag {

            final boolean fatal;
            final String message;

            ModifierDiagnostic(List<Modifier> list, boolean fatal) {
                this.fatal = fatal;
                StringBuilder sb = new StringBuilder();
                for (Modifier mod : list) {
                    sb.append("'");
                    sb.append(mod.toString());
                    sb.append("' ");
                }
                String key = (list.size() > 1)
                        ? fatal
                            ? "jshell.diag.modifier.plural.fatal"
                            : "jshell.diag.modifier.plural.ignore"
                        : fatal
                            ? "jshell.diag.modifier.single.fatal"
                            : "jshell.diag.modifier.single.ignore";
                this.message = state.messageFormat(key, sb.toString());
            }

            @Override
            public boolean isError() {
                return fatal;
            }

            @Override
            public long getPosition() {
                return dis.getStartPosition(modtree);
            }

            @Override
            public long getStartPosition() {
                return dis.getStartPosition(modtree);
            }

            @Override
            public long getEndPosition() {
                return dis.getEndPosition(modtree);
            }

            @Override
            public String getCode() {
                return fatal
                        ? "jdk.eval.error.illegal.modifiers"
                        : "jdk.eval.warn.illegal.modifiers";
            }

            @Override
            public String getMessage(Locale locale) {
                return message;
            }
        }

        List<Modifier> list = new ArrayList<>();
        boolean fatal = false;
        for (Modifier mod : modtree.getFlags()) {
            switch (mod) {
                case SYNCHRONIZED:
                case NATIVE:
                    list.add(mod);
                    fatal = true;
                    break;
                case ABSTRACT:
                    if (isAbstractProhibited) {
                        list.add(mod);
                        fatal = true;
                    }
                    break;
                case PUBLIC:
                case PROTECTED:
                case PRIVATE:
                    // quietly ignore, user cannot see effects one way or the other
                    break;
                case STATIC:
                case FINAL:
                    list.add(mod);
                    break;
            }
        }
        return list.isEmpty()
                ? new DiagList()
                : new DiagList(new ModifierDiagnostic(list, fatal));
    }

}
