/*
 * Copyright (c) 2012, 2013, Oracle and/or its affiliates. All rights reserved.
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
package java.util.stream;

import java.nio.charset.Charset;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;
import java.util.Collection;
import java.util.DoubleSummaryStatistics;
import java.util.Objects;
import java.util.OptionalDouble;
import java.util.PrimitiveIterator;
import java.util.Spliterator;
import java.util.Spliterators;
import java.util.concurrent.ConcurrentHashMap;
import java.util.function.BiConsumer;
import java.util.function.DoubleBinaryOperator;
import java.util.function.DoubleConsumer;
import java.util.function.DoubleFunction;
import java.util.function.DoublePredicate;
import java.util.function.DoubleSupplier;
import java.util.function.DoubleToIntFunction;
import java.util.function.DoubleToLongFunction;
import java.util.function.DoubleUnaryOperator;
import java.util.function.Function;
import java.util.function.ObjDoubleConsumer;
import java.util.function.Supplier;

/**
 * A sequence of elements supporting sequential and parallel aggregate
 * operations.  The following example illustrates an aggregate operation using
 * {@link Stream} and {@link DoubleStream}:
 *
 * <pre>{@code
 *     double sum = widgets.stream()
 *                         .filter(w -> w.getColor() == RED)
 *                         .mapToDouble(w -> w.getWeight())
 *                         .sum();
 * }</pre>
 *
 * In this example, {@code widgets} is a {@code Collection<Widget>}.  We create
 * a stream of {@code Widget} objects via {@link Collection#stream Collection.stream()},
 * filter it to produce a stream containing only the red widgets, and then
 * transform it into a stream of {@code double} values representing the weight of
 * each red widget. Then this stream is summed to produce a total weight.
 *
 * <p>To perform a computation, stream
 * <a href="package-summary.html#StreamOps">operations</a> are composed into a
 * <em>stream pipeline</em>.  A stream pipeline consists of a source (which
 * might be an array, a collection, a generator function, an IO channel,
 * etc), zero or more <em>intermediate operations</em> (which transform a
 * stream into another stream, such as {@link DoubleStream#filter(DoublePredicate)}), and a
 * <em>terminal operation</em> (which produces a result or side-effect, such
 * as {@link DoubleStream#sum()} or {@link DoubleStream#forEach(DoubleConsumer)}.
 * Streams are lazy; computation on the source data is only performed when the
 * terminal operation is initiated, and source elements are consumed only
 * as needed.
 *
 * <p>Collections and streams, while bearing some superficial similarities,
 * have different goals.  Collections are primarily concerned with the efficient
 * management of, and access to, their elements.  By contrast, streams do not
 * provide a means to directly access or manipulate their elements, and are
 * instead concerned with declaratively describing their source and the
 * computational operations which will be performed in aggregate on that source.
 * However, if the provided stream operations do not offer the desired
 * functionality, the {@link #iterator()} and {@link #spliterator()} operations
 * can be used to perform a controlled traversal.
 *
 * <p>A stream pipeline, like the "widgets" example above, can be viewed as
 * a <em>query</em> on the stream source.  Unless the source was explicitly
 * designed for concurrent modification (such as a {@link ConcurrentHashMap}),
 * unpredictable or erroneous behavior may result from modifying the stream
 * source while it is being queried.
 *
 * <p>Most stream operations accept parameters that describe user-specified
 * behavior, such as the lambda expression {@code w -> w.getWeight()} passed to
 * {@code mapToDouble} in the example above.  Such parameters are always instances
 * of a <a href="../function/package-summary.html">functional interface</a> such
 * as {@link java.util.function.Function}, and are often lambda expressions or
 * method references.  These parameters can never be null, should not modify the
 * stream source, and should be
 * <a href="package-summary.html#NonInterference">effectively stateless</a>
 * (their result should not depend on any state that might change during
 * execution of the stream pipeline.)
 *
 * <p>A stream should be operated on (invoking an intermediate or terminal stream
 * operation) only once.  This rules out, for example, "forked" streams, where
 * the same source feeds two or more pipelines, or multiple traversals of the
 * same stream.  A stream implementation may throw {@link IllegalStateException}
 * if it detects that the stream is being reused. However, since some stream
 * operations may return their receiver rather than a new stream object, it may
 * not be possible to detect reuse in all cases.
 *
 * <p>Streams have a {@link #close()} method and implement {@link AutoCloseable},
 * but nearly all stream instances do not actually need to be closed after use.
 * Generally, only streams whose source is an IO channel (such as those returned
 * by {@link Files#lines(Path, Charset)}) will require closing.  Most streams
 * are backed by collections, arrays, or generating functions, which require no
 * special resource management.  (If a stream does require closing, it can be
 * declared as a resource in a {@code try}-with-resources statement.)
 *
 * <p>Stream pipelines may execute either sequentially or in
 * <a href="package-summary.html#Parallelism">parallel</a>.  This
 * execution mode is a property of the stream.  Streams are created
 * with an initial choice of sequential or parallel execution.  (For example,
 * {@link Collection#stream() Collection.stream()} creates a sequential stream,
 * and {@link Collection#parallelStream() Collection.parallelStream()} creates
 * a parallel one.)  This choice of execution mode may be modified by the
 * {@link #sequential()} or {@link #parallel()} methods, and may be queried with
 * the {@link #isParallel()} method.
 *
 * @since 1.8
 * @see <a href="package-summary.html">java.util.stream</a>
 */
public interface DoubleStream extends BaseStream<Double, DoubleStream> {

    /**
     * Returns a stream consisting of the elements of this stream that match
     * the given predicate.
     *
     * <p>This is an <a href="package-summary.html#StreamOps">intermediate
     * operation</a>.
     *
     * @param predicate a <a href="package-summary.html#NonInterference">
     *                  non-interfering, stateless</a> predicate to apply to
     *                  each element to determine if it should be included
     * @return the new stream
     */
    DoubleStream filter(DoublePredicate predicate);

    /**
     * Returns a stream consisting of the results of applying the given
     * function to the elements of this stream.
     *
     * <p>This is an <a href="package-summary.html#StreamOps">intermediate
     * operation</a>.
     *
     * @param mapper a <a href="package-summary.html#NonInterference">
     *               non-interfering, stateless</a> function to apply to
     *               each element
     * @return the new stream
     */
    DoubleStream map(DoubleUnaryOperator mapper);

    /**
     * Returns an object-valued {@code Stream} consisting of the results of
     * applying the given function to the elements of this stream.
     *
     * <p>This is an <a href="package-summary.html#StreamOps">
     *     intermediate operation</a>.
     *
     * @param <U> the element type of the new stream
     * @param mapper a <a href="package-summary.html#NonInterference">
     *               non-interfering, stateless</a> function to apply to each
     *               element
     * @return the new stream
     */
    <U> Stream<U> mapToObj(DoubleFunction<? extends U> mapper);

    /**
     * Returns an {@code IntStream} consisting of the results of applying the
     * given function to the elements of this stream.
     *
     * <p>This is an <a href="package-summary.html#StreamOps">intermediate
     * operation</a>.
     *
     * @param mapper a <a href="package-summary.html#NonInterference">
     *               non-interfering, stateless</a> function to apply to each
     *               element
     * @return the new stream
     */
    IntStream mapToInt(DoubleToIntFunction mapper);

    /**
     * Returns a {@code LongStream} consisting of the results of applying the
     * given function to the elements of this stream.
     *
     * <p>This is an <a href="package-summary.html#StreamOps">intermediate
     * operation</a>.
     *
     * @param mapper a <a href="package-summary.html#NonInterference">
     *               non-interfering, stateless</a> function to apply to each
     *               element
     * @return the new stream
     */
    LongStream mapToLong(DoubleToLongFunction mapper);

    /**
     * Returns a stream consisting of the results of replacing each element of
     * this stream with the contents of the stream produced by applying the
     * provided mapping function to each element.  (If the result of the mapping
     * function is {@code null}, this is treated as if the result was an empty
     * stream.)
     *
     * <p>This is an <a href="package-summary.html#StreamOps">intermediate
     * operation</a>.
     *
     * @param mapper a <a href="package-summary.html#NonInterference">
     *               non-interfering, stateless</a> function to apply to
     *               each element which produces an {@code DoubleStream} of new
     *               values
     * @return the new stream
     * @see Stream#flatMap(Function)
     */
    DoubleStream flatMap(DoubleFunction<? extends DoubleStream> mapper);

    /**
     * Returns a stream consisting of the distinct elements of this stream. The
     * elements are compared for equality according to
     * {@link java.lang.Double#compare(double, double)}.
     *
     * <p>This is a <a href="package-summary.html#StreamOps">stateful
     * intermediate operation</a>.
     *
     * @return the result stream
     */
    DoubleStream distinct();

    /**
     * Returns a stream consisting of the elements of this stream in sorted
     * order. The elements are compared for equality according to
     * {@link java.lang.Double#compare(double, double)}.
     *
     * <p>This is a <a href="package-summary.html#StreamOps">stateful
     * intermediate operation</a>.
     *
     * @return the result stream
     */
    DoubleStream sorted();

    /**
     * Returns a stream consisting of the elements of this stream, additionally
     * performing the provided action on each element as elements are consumed
     * from the resulting stream.
     *
     * <p>This is an <a href="package-summary.html#StreamOps">intermediate
     * operation</a>.
     *
     * <p>For parallel stream pipelines, the action may be called at
     * whatever time and in whatever thread the element is made available by the
     * upstream operation.  If the action modifies shared state,
     * it is responsible for providing the required synchronization.
     *
     * @apiNote This method exists mainly to support debugging, where you want
     * to see the elements as they flow past a certain point in a pipeline:
     * <pre>{@code
     *     list.stream()
     *         .filter(filteringFunction)
     *         .peek(e -> System.out.println("Filtered value: " + e));
     *         .map(mappingFunction)
     *         .peek(e -> System.out.println("Mapped value: " + e));
     *         .collect(Collectors.toDoubleSummaryStastistics());
     * }</pre>
     *
     * @param action a <a href="package-summary.html#NonInterference">
     *                 non-interfering</a> action to perform on the elements as
     *                 they are consumed from the stream
     * @return the new stream
     */
    DoubleStream peek(DoubleConsumer action);

    /**
     * Returns a stream consisting of the elements of this stream, truncated
     * to be no longer than {@code maxSize} in length.
     *
     * <p>This is a <a href="package-summary.html#StreamOps">short-circuiting
     * stateful intermediate operation</a>.
     *
     * @param maxSize the number of elements the stream should be limited to
     * @return the new stream
     * @throws IllegalArgumentException if {@code maxSize} is negative
     */
    DoubleStream limit(long maxSize);

    /**
     * Returns a stream consisting of the remaining elements of this stream
     * after discarding the first {@code startInclusive} elements of the stream.
     * If this stream contains fewer than {@code startInclusive} elements then an
     * empty stream will be returned.
     *
     * <p>This is a <a href="package-summary.html#StreamOps">stateful
     * intermediate operation</a>.
     *
     * @param startInclusive the number of leading elements to skip
     * @return the new stream
     * @throws IllegalArgumentException if {@code startInclusive} is negative
     */
    DoubleStream substream(long startInclusive);

    /**
     * Returns a stream consisting of the remaining elements of this stream
     * after discarding the first {@code startInclusive} elements and truncating
     * the result to be no longer than {@code endExclusive - startInclusive}
     * elements in length. If this stream contains fewer than
     * {@code startInclusive} elements then an empty stream will be returned.
     *
     * <p>This is a <a href="package-summary.html#StreamOps">short-circuiting
     * stateful intermediate operation</a>.
     *
     * @param startInclusive the starting position of the substream, inclusive
     * @param endExclusive the ending position of the substream, exclusive
     * @return the new stream
     * @throws IllegalArgumentException if {@code startInclusive} or
     * {@code endExclusive} is negative or {@code startInclusive} is greater
     * than {@code endExclusive}
     */
    DoubleStream substream(long startInclusive, long endExclusive);

    /**
     * Performs an action for each element of this stream.
     *
     * <p>This is a <a href="package-summary.html#StreamOps">terminal
     * operation</a>.
     *
     * <p>For parallel stream pipelines, this operation does <em>not</em>
     * guarantee to respect the encounter order of the stream, as doing so
     * would sacrifice the benefit of parallelism.  For any given element, the
     * action may be performed at whatever time and in whatever thread the
     * library chooses.  If the action accesses shared state, it is
     * responsible for providing the required synchronization.
     *
     * @param action a <a href="package-summary.html#NonInterference">
     *               non-interfering</a> action to perform on the elements
     */
    void forEach(DoubleConsumer action);

    /**
     * Performs an action for each element of this stream, guaranteeing that
     * each element is processed in encounter order for streams that have a
     * defined encounter order.
     *
     * <p>This is a <a href="package-summary.html#StreamOps">terminal
     * operation</a>.
     *
     * @param action a <a href="package-summary.html#NonInterference">
     *               non-interfering</a> action to perform on the elements
     * @see #forEach(DoubleConsumer)
     */
    void forEachOrdered(DoubleConsumer action);

    /**
     * Returns an array containing the elements of this stream.
     *
     * <p>This is a <a href="package-summary.html#StreamOps">terminal
     * operation</a>.
     *
     * @return an array containing the elements of this stream
     */
    double[] toArray();

    /**
     * Performs a <a href="package-summary.html#Reduction">reduction</a> on the
     * elements of this stream, using the provided identity value and an
     * <a href="package-summary.html#Associativity">associative</a>
     * accumulation function, and returns the reduced value.  This is equivalent
     * to:
     * <pre>{@code
     *     double result = identity;
     *     for (double element : this stream)
     *         result = accumulator.apply(result, element)
     *     return result;
     * }</pre>
     *
     * but is not constrained to execute sequentially.
     *
     * <p>The {@code identity} value must be an identity for the accumulator
     * function. This means that for all {@code x},
     * {@code accumulator.apply(identity, x)} is equal to {@code x}.
     * The {@code accumulator} function must be an
     * <a href="package-summary.html#Associativity">associative</a> function.
     *
     * <p>This is a <a href="package-summary.html#StreamOps">terminal
     * operation</a>.
     *
     * @apiNote Sum, min, max, and average are all special cases of reduction.
     * Summing a stream of numbers can be expressed as:

     * <pre>{@code
     *     double sum = numbers.reduce(0, (a, b) -> a+b);
     * }</pre>
     *
     * or more compactly:
     *
     * <pre>{@code
     *     double sum = numbers.reduce(0, Double::sum);
     * }</pre>
     *
     * <p>While this may seem a more roundabout way to perform an aggregation
     * compared to simply mutating a running total in a loop, reduction
     * operations parallelize more gracefully, without needing additional
     * synchronization and with greatly reduced risk of data races.
     *
     * @param identity the identity value for the accumulating function
     * @param op an <a href="package-summary.html#Associativity">associative</a>
     *                    <a href="package-summary.html#NonInterference">non-interfering,
     *                    stateless</a> function for combining two values
     * @return the result of the reduction
     * @see #sum()
     * @see #min()
     * @see #max()
     * @see #average()
     */
    double reduce(double identity, DoubleBinaryOperator op);

    /**
     * Performs a <a href="package-summary.html#Reduction">reduction</a> on the
     * elements of this stream, using an
     * <a href="package-summary.html#Associativity">associative</a> accumulation
     * function, and returns an {@code OptionalDouble} describing the reduced
     * value, if any. This is equivalent to:
     * <pre>{@code
     *     boolean foundAny = false;
     *     double result = null;
     *     for (double element : this stream) {
     *         if (!foundAny) {
     *             foundAny = true;
     *             result = element;
     *         }
     *         else
     *             result = accumulator.apply(result, element);
     *     }
     *     return foundAny ? OptionalDouble.of(result) : OptionalDouble.empty();
     * }</pre>
     *
     * but is not constrained to execute sequentially.
     *
     * <p>The {@code accumulator} function must be an
     * <a href="package-summary.html#Associativity">associative</a> function.
     *
     * <p>This is a <a href="package-summary.html#StreamOps">terminal
     * operation</a>.
     *
     * @param op an <a href="package-summary.html#Associativity">associative</a>
     *           <a href="package-summary.html#NonInterference">non-interfering,
     *           stateless</a> function for combining two values
     * @return the result of the reduction
     * @see #reduce(double, DoubleBinaryOperator)
     */
    OptionalDouble reduce(DoubleBinaryOperator op);

    /**
     * Performs a <a href="package-summary.html#MutableReduction">mutable
     * reduction</a> operation on the elements of this stream.  A mutable
     * reduction is one in which the reduced value is a mutable result container,
     * such as an {@code ArrayList}, and elements are incorporated by updating
     * the state of the result rather than by replacing the result.  This
     * produces a result equivalent to:
     * <pre>{@code
     *     R result = supplier.get();
     *     for (double element : this stream)
     *         accumulator.accept(result, element);
     *     return result;
     * }</pre>
     *
     * <p>Like {@link #reduce(double, DoubleBinaryOperator)}, {@code collect}
     * operations can be parallelized without requiring additional
     * synchronization.
     *
     * <p>This is a <a href="package-summary.html#StreamOps">terminal
     * operation</a>.
     *
     * @param <R> type of the result
     * @param supplier a function that creates a new result container. For a
     *                 parallel execution, this function may be called
     *                 multiple times and must return a fresh value each time.
     * @param accumulator an <a href="package-summary.html#Associativity">associative</a>
     *                    <a href="package-summary.html#NonInterference">non-interfering,
     *                    stateless</a> function for incorporating an additional
     *                    element into a result
     * @param combiner an <a href="package-summary.html#Associativity">associative</a>
     *                 <a href="package-summary.html#NonInterference">non-interfering,
     *                 stateless</a> function for combining two values, which
     *                 must be compatible with the accumulator function
     * @return the result of the reduction
     * @see Stream#collect(Supplier, BiConsumer, BiConsumer)
     */
    <R> R collect(Supplier<R> supplier,
                  ObjDoubleConsumer<R> accumulator,
                  BiConsumer<R, R> combiner);

    /**
     * Returns the sum of elements in this stream.  The sum returned can vary
     * depending upon the order in which elements are encountered.  This is due
     * to accumulated rounding error in addition of values of differing
     * magnitudes. Elements sorted by increasing absolute magnitude tend to
     * yield more accurate results.  If any stream element is a {@code NaN} or
     * the sum is at any point a {@code NaN} then the sum will be {@code NaN}.
     * This is a special case of a
     * <a href="package-summary.html#Reduction">reduction</a> and is
     * equivalent to:
     * <pre>{@code
     *     return reduce(0, Double::sum);
     * }</pre>
     *
     * <p>This is a <a href="package-summary.html#StreamOps">terminal
     * operation</a>.
     *
     * @return the sum of elements in this stream
     */
    double sum();

    /**
     * Returns an {@code OptionalDouble} describing the minimum element of this
     * stream, or an empty OptionalDouble if this stream is empty.  The minimum
     * element will be {@code Double.NaN} if any stream element was NaN. Unlike
     * the numerical comparison operators, this method considers negative zero
     * to be strictly smaller than positive zero. This is a special case of a
     * <a href="package-summary.html#Reduction">reduction</a> and is
     * equivalent to:
     * <pre>{@code
     *     return reduce(Double::min);
     * }</pre>
     *
     * <p>This is a <a href="package-summary.html#StreamOps">terminal
     * operation</a>.
     *
     * @return an {@code OptionalDouble} containing the minimum element of this
     * stream, or an empty optional if the stream is empty
     */
    OptionalDouble min();

    /**
     * Returns an {@code OptionalDouble} describing the maximum element of this
     * stream, or an empty OptionalDouble if this stream is empty.  The maximum
     * element will be {@code Double.NaN} if any stream element was NaN. Unlike
     * the numerical comparison operators, this method considers negative zero
     * to be strictly smaller than positive zero. This is a
     * special case of a
     * <a href="package-summary.html#Reduction">reduction</a> and is
     * equivalent to:
     * <pre>{@code
     *     return reduce(Double::max);
     * }</pre>
     *
     * <p>This is a <a href="package-summary.html#StreamOps">terminal
     * operation</a>.
     *
     * @return an {@code OptionalDouble} containing the maximum element of this
     * stream, or an empty optional if the stream is empty
     */
    OptionalDouble max();

    /**
     * Returns the count of elements in this stream.  This is a special case of
     * a <a href="package-summary.html#Reduction">reduction</a> and is
     * equivalent to:
     * <pre>{@code
     *     return mapToLong(e -> 1L).sum();
     * }</pre>
     *
     * <p>This is a <a href="package-summary.html#StreamOps">terminal operation</a>.
     *
     * @return the count of elements in this stream
     */
    long count();

    /**
     * Returns an {@code OptionalDouble} describing the arithmetic mean of elements of
     * this stream, or an empty optional if this stream is empty.  The average
     * returned can vary depending upon the order in which elements are
     * encountered. This is due to accumulated rounding error in addition of
     * elements of differing magnitudes. Elements sorted by increasing absolute
     * magnitude tend to yield more accurate results. If any recorded value is
     * a {@code NaN} or the sum is at any point a {@code NaN} then the average
     * will be {@code NaN}. This is a special case of a
     * <a href="package-summary.html#Reduction">reduction</a>.
     *
     * <p>This is a <a href="package-summary.html#StreamOps">terminal
     * operation</a>.
     *
     * @return an {@code OptionalDouble} containing the average element of this
     * stream, or an empty optional if the stream is empty
     */
    OptionalDouble average();

    /**
     * Returns a {@code DoubleSummaryStatistics} describing various summary data
     * about the elements of this stream.  This is a special
     * case of a <a href="package-summary.html#Reduction">reduction</a>.
     *
     * <p>This is a <a href="package-summary.html#StreamOps">terminal
     * operation</a>.
     *
     * @return a {@code DoubleSummaryStatistics} describing various summary data
     * about the elements of this stream
     */
    DoubleSummaryStatistics summaryStatistics();

    /**
     * Returns whether any elements of this stream match the provided
     * predicate.  May not evaluate the predicate on all elements if not
     * necessary for determining the result.
     *
     * <p>This is a <a href="package-summary.html#StreamOps">short-circuiting
     * terminal operation</a>.
     *
     * @param predicate a <a href="package-summary.html#NonInterference">non-interfering,
     *                  stateless</a> predicate to apply to elements of this
     *                  stream
     * @return {@code true} if any elements of the stream match the provided
     * predicate otherwise {@code false}
     */
    boolean anyMatch(DoublePredicate predicate);

    /**
     * Returns whether all elements of this stream match the provided predicate.
     * May not evaluate the predicate on all elements if not necessary for
     * determining the result.
     *
     * <p>This is a <a href="package-summary.html#StreamOps">short-circuiting
     * terminal operation</a>.
     *
     * @param predicate a <a href="package-summary.html#NonInterference">non-interfering,
     *                  stateless</a> predicate to apply to elements of this
     *                  stream
     * @return {@code true} if all elements of the stream match the provided
     * predicate otherwise {@code false}
     */
    boolean allMatch(DoublePredicate predicate);

    /**
     * Returns whether no elements of this stream match the provided predicate.
     * May not evaluate the predicate on all elements if not necessary for
     * determining the result.
     *
     * <p>This is a <a href="package-summary.html#StreamOps">short-circuiting
     * terminal operation</a>.
     *
     * @param predicate a <a href="package-summary.html#NonInterference">non-interfering,
     *                  stateless</a> predicate to apply to elements of this
     *                  stream
     * @return {@code true} if no elements of the stream match the provided
     * predicate otherwise {@code false}
     */
    boolean noneMatch(DoublePredicate predicate);

    /**
     * Returns an {@link OptionalDouble} describing the first element of this
     * stream, or an empty {@code OptionalDouble} if the stream is empty.  If
     * the stream has no encounter order, then any element may be returned.
     *
     * <p>This is a <a href="package-summary.html#StreamOps">short-circuiting
     * terminal operation</a>.
     *
     * @return an {@code OptionalDouble} describing the first element of this
     * stream, or an empty {@code OptionalDouble} if the stream is empty
     */
    OptionalDouble findFirst();

    /**
     * Returns an {@link OptionalDouble} describing some element of the stream,
     * or an empty {@code OptionalDouble} if the stream is empty.
     *
     * <p>This is a <a href="package-summary.html#StreamOps">short-circuiting
     * terminal operation</a>.
     *
     * <p>The behavior of this operation is explicitly nondeterministic; it is
     * free to select any element in the stream.  This is to allow for maximal
     * performance in parallel operations; the cost is that multiple invocations
     * on the same source may not return the same result.  (If a stable result
     * is desired, use {@link #findFirst()} instead.)
     *
     * @return an {@code OptionalDouble} describing some element of this stream,
     * or an empty {@code OptionalDouble} if the stream is empty
     * @see #findFirst()
     */
    OptionalDouble findAny();

    /**
     * Returns a {@code Stream} consisting of the elements of this stream,
     * boxed to {@code Double}.
     *
     * <p>This is an <a href="package-summary.html#StreamOps">intermediate
     * operation</a>.
     *
     * @return a {@code Stream} consistent of the elements of this stream,
     * each boxed to a {@code Double}
     */
    Stream<Double> boxed();

    @Override
    DoubleStream sequential();

    @Override
    DoubleStream parallel();

    @Override
    PrimitiveIterator.OfDouble iterator();

    @Override
    Spliterator.OfDouble spliterator();


    // Static factories

    /**
     * Returns a builder for a {@code DoubleStream}.
     *
     * @return a stream builder
     */
    public static Builder builder() {
        return new Streams.DoubleStreamBuilderImpl();
    }

    /**
     * Returns an empty sequential {@code DoubleStream}.
     *
     * @return an empty sequential stream
     */
    public static DoubleStream empty() {
        return StreamSupport.doubleStream(Spliterators.emptyDoubleSpliterator(), false);
    }

    /**
     * Returns a sequential {@code DoubleStream} containing a single element.
     *
     * @param t the single element
     * @return a singleton sequential stream
     */
    public static DoubleStream of(double t) {
        return StreamSupport.doubleStream(new Streams.DoubleStreamBuilderImpl(t), false);
    }

    /**
     * Returns a sequential ordered stream whose elements are the specified values.
     *
     * @param values the elements of the new stream
     * @return the new stream
     */
    public static DoubleStream of(double... values) {
        return Arrays.stream(values);
    }

    /**
     * Returns an infinite sequential ordered {@code DoubleStream} produced by iterative
     * application of a function {@code f} to an initial element {@code seed},
     * producing a {@code Stream} consisting of {@code seed}, {@code f(seed)},
     * {@code f(f(seed))}, etc.
     *
     * <p>The first element (position {@code 0}) in the {@code DoubleStream}
     * will be the provided {@code seed}.  For {@code n > 0}, the element at
     * position {@code n}, will be the result of applying the function {@code f}
     *  to the element at position {@code n - 1}.
     *
     * @param seed the initial element
     * @param f a function to be applied to to the previous element to produce
     *          a new element
     * @return a new sequential {@code DoubleStream}
     */
    public static DoubleStream iterate(final double seed, final DoubleUnaryOperator f) {
        Objects.requireNonNull(f);
        final PrimitiveIterator.OfDouble iterator = new PrimitiveIterator.OfDouble() {
            double t = seed;

            @Override
            public boolean hasNext() {
                return true;
            }

            @Override
            public double nextDouble() {
                double v = t;
                t = f.applyAsDouble(t);
                return v;
            }
        };
        return StreamSupport.doubleStream(Spliterators.spliteratorUnknownSize(
                iterator,
                Spliterator.ORDERED | Spliterator.IMMUTABLE | Spliterator.NONNULL), false);
    }

    /**
     * Returns a sequential stream where each element is generated by
     * the provided {@code DoubleSupplier}.  This is suitable for generating
     * constant streams, streams of random elements, etc.
     *
     * @param s the {@code DoubleSupplier} for generated elements
     * @return a new sequential {@code DoubleStream}
     */
    public static DoubleStream generate(DoubleSupplier s) {
        Objects.requireNonNull(s);
        return StreamSupport.doubleStream(
                new StreamSpliterators.InfiniteSupplyingSpliterator.OfDouble(Long.MAX_VALUE, s), false);
    }

    /**
     * Creates a lazily concatenated stream whose elements are all the
     * elements of the first stream followed by all the elements of the
     * second stream. The resulting stream is ordered if both
     * of the input streams are ordered, and parallel if either of the input
     * streams is parallel.  When the resulting stream is closed, the close
     * handlers for both input streams are invoked.
     *
     * @param a the first stream
     * @param b the second stream
     * @return the concatenation of the two input streams
     */
    public static DoubleStream concat(DoubleStream a, DoubleStream b) {
        Objects.requireNonNull(a);
        Objects.requireNonNull(b);

        Spliterator.OfDouble split = new Streams.ConcatSpliterator.OfDouble(
                a.spliterator(), b.spliterator());
        DoubleStream stream = StreamSupport.doubleStream(split, a.isParallel() || b.isParallel());
        return stream.onClose(Streams.composedClose(a, b));
    }

    /**
     * A mutable builder for a {@code DoubleStream}.
     *
     * <p>A stream builder has a lifecycle, which starts in a building
     * phase, during which elements can be added, and then transitions to a built
     * phase, after which elements may not be added.  The built phase
     * begins when the {@link #build()} method is called, which creates an
     * ordered stream whose elements are the elements that were added to the
     * stream builder, in the order they were added.
     *
     * @see DoubleStream#builder()
     * @since 1.8
     */
    public interface Builder extends DoubleConsumer {

        /**
         * Adds an element to the stream being built.
         *
         * @throws IllegalStateException if the builder has already transitioned
         * to the built state
         */
        @Override
        void accept(double t);

        /**
         * Adds an element to the stream being built.
         *
         * @implSpec
         * The default implementation behaves as if:
         * <pre>{@code
         *     accept(t)
         *     return this;
         * }</pre>
         *
         * @param t the element to add
         * @return {@code this} builder
         * @throws IllegalStateException if the builder has already transitioned
         * to the built state
         */
        default Builder add(double t) {
            accept(t);
            return this;
        }

        /**
         * Builds the stream, transitioning this builder to the built state.
         * An {@code IllegalStateException} is thrown if there are further
         * attempts to operate on the builder after it has entered the built
         * state.
         *
         * @return the built stream
         * @throws IllegalStateException if the builder has already transitioned
         * to the built state
         */
        DoubleStream build();
    }
}
