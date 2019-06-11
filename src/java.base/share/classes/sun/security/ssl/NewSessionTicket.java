/*
 * Copyright (c) 2018, 2019, Oracle and/or its affiliates. All rights reserved.
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
package sun.security.ssl;

import java.io.IOException;
import java.math.BigInteger;
import java.nio.ByteBuffer;
import java.security.GeneralSecurityException;
import java.security.SecureRandom;
import java.text.MessageFormat;
import java.util.Locale;
import javax.crypto.SecretKey;
import javax.net.ssl.SSLHandshakeException;
import sun.security.ssl.PskKeyExchangeModesExtension.PskKeyExchangeModesSpec;
import sun.security.ssl.SessionTicketExtension.SessionTicketSpec;
import sun.security.ssl.SSLHandshake.HandshakeMessage;
import sun.security.util.HexDumpEncoder;

import static sun.security.ssl.SSLHandshake.NEW_SESSION_TICKET;

/**
 * Pack of the NewSessionTicket handshake message.
 */
final class NewSessionTicket {
    static final int MAX_TICKET_LIFETIME = 604800;  // seconds, 7 days

    static final SSLConsumer handshakeConsumer =
        new T13NewSessionTicketConsumer();
    static final SSLConsumer handshake12Consumer =
        new T12NewSessionTicketConsumer();
    static final SSLProducer kickstartProducer =
        new NewSessionTicketKickstartProducer();
    static final HandshakeProducer handshake12Producer =
        new T12NewSessionTicketProducer();

    /**
     * The NewSessionTicketMessage handshake messages.
     */
    abstract static class NewSessionTicketMessage extends HandshakeMessage {
        int ticketLifetime;
        byte[] ticket;

        NewSessionTicketMessage(HandshakeContext context) {
            super(context);
        }

        @Override
        public SSLHandshake handshakeType() {
            return NEW_SESSION_TICKET;
        }

        // For TLS 1.3 only
        int getTicketAgeAdd() throws IOException {
            throw handshakeContext.conContext.fatal(Alert.ILLEGAL_PARAMETER,
                    "TicketAgeAdd not part of RFC 5077.");
        }

        // For TLS 1.3 only
        byte[] getTicketNonce() throws IOException {
            throw handshakeContext.conContext.fatal(Alert.ILLEGAL_PARAMETER,
                    "TicketNonce not part of RFC 5077.");
        }

    }
    /**
     * NewSessionTicket for TLS 1.2 and below (RFC 5077)
     */
    static final class T12NewSessionTicketMessage extends NewSessionTicketMessage {

        T12NewSessionTicketMessage(HandshakeContext context,
                int ticketLifetime, byte[] ticket) {
            super(context);

            this.ticketLifetime = ticketLifetime;
            this.ticket = ticket;
        }

        T12NewSessionTicketMessage(HandshakeContext context,
                ByteBuffer m) throws IOException {

            // RFC5077 struct {
            //     uint32 ticket_lifetime;
            //     opaque ticket<1..2^16-1>;
            // } NewSessionTicket;

            super(context);
            if (m.remaining() < 14) {
                throw context.conContext.fatal(Alert.ILLEGAL_PARAMETER,
                        "Invalid NewSessionTicket message: no sufficient data");
            }

            this.ticketLifetime = Record.getInt32(m);
            this.ticket = Record.getBytes16(m);
        }

        @Override
        public SSLHandshake handshakeType() {
            return NEW_SESSION_TICKET;
        }

        @Override
        public int messageLength() {
            return 4 + // ticketLifetime
                    2 + ticket.length;  // len of ticket + ticket
        }

        @Override
        public void send(HandshakeOutStream hos) throws IOException {
            hos.putInt32(ticketLifetime);
            hos.putBytes16(ticket);
        }

        @Override
        public String toString() {
            MessageFormat messageFormat = new MessageFormat(
                    "\"NewSessionTicket\": '{'\n" +
                            "  \"ticket_lifetime\"      : \"{0}\",\n" +
                            "  \"ticket\"               : '{'\n" +
                            "{1}\n" +
                            "  '}'" +
                            "'}'",
                Locale.ENGLISH);

            HexDumpEncoder hexEncoder = new HexDumpEncoder();
            Object[] messageFields = {
                    ticketLifetime,
                    Utilities.indent(hexEncoder.encode(ticket), "    "),
            };
            return messageFormat.format(messageFields);
        }
    }

    /**
     * NewSessionTicket defined by the TLS 1.3
     */
    static final class T13NewSessionTicketMessage extends NewSessionTicketMessage {
        int ticketAgeAdd;
        byte[] ticketNonce;
        SSLExtensions extensions;

        T13NewSessionTicketMessage(HandshakeContext context,
                int ticketLifetime, SecureRandom generator,
                byte[] ticketNonce, byte[] ticket) {
            super(context);

            this.ticketLifetime = ticketLifetime;
            this.ticketAgeAdd = generator.nextInt();
            this.ticketNonce = ticketNonce;
            this.ticket = ticket;
            this.extensions = new SSLExtensions(this);
        }

        T13NewSessionTicketMessage(HandshakeContext context,
                ByteBuffer m) throws IOException {
            super(context);

            // struct {
            //     uint32 ticket_lifetime;
            //     uint32 ticket_age_add;
            //     opaque ticket_nonce<0..255>;
            //     opaque ticket<1..2^16-1>;
            //     Extension extensions<0..2^16-2>;
            // } NewSessionTicket;

            if (m.remaining() < 14) {
                throw context.conContext.fatal(Alert.ILLEGAL_PARAMETER,
                    "Invalid NewSessionTicket message: no sufficient data");
            }

            this.ticketLifetime = Record.getInt32(m);
            this.ticketAgeAdd = Record.getInt32(m);
            this.ticketNonce = Record.getBytes8(m);

            if (m.remaining() < 5) {
                throw context.conContext.fatal(Alert.ILLEGAL_PARAMETER,
                    "Invalid NewSessionTicket message: no sufficient data");
            }

            this.ticket = Record.getBytes16(m);
            if (ticket.length == 0) {
                throw context.conContext.fatal(Alert.ILLEGAL_PARAMETER,
                    "No ticket in the NewSessionTicket handshake message");
            }

            if (m.remaining() < 2) {
                throw context.conContext.fatal(Alert.ILLEGAL_PARAMETER,
                    "Invalid NewSessionTicket message: no sufficient data");
            }

            SSLExtension[] supportedExtensions =
                    context.sslConfig.getEnabledExtensions(
                            NEW_SESSION_TICKET);
            this.extensions = new SSLExtensions(this, m, supportedExtensions);
        }

        @Override
        public SSLHandshake handshakeType() {
            return NEW_SESSION_TICKET;
        }

        int getTicketAgeAdd() {
            return ticketAgeAdd;
        }

        byte[] getTicketNonce() {
            return ticketNonce;
        }

        @Override
        public int messageLength() {

            int extLen = extensions.length();
            if (extLen == 0) {
                extLen = 2;     // empty extensions
            }

            return 4 +// ticketLifetime
                    4 + // ticketAgeAdd
                    1 + ticketNonce.length + // len of nonce + nonce
                    2 + ticket.length + // len of ticket + ticket
                    extLen;
        }

        @Override
        public void send(HandshakeOutStream hos) throws IOException {
            hos.putInt32(ticketLifetime);
            hos.putInt32(ticketAgeAdd);
            hos.putBytes8(ticketNonce);
            hos.putBytes16(ticket);

            // Is it an empty extensions?
            if (extensions.length() == 0) {
                hos.putInt16(0);
            } else {
                extensions.send(hos);
            }
        }

        @Override
        public String toString() {
            MessageFormat messageFormat = new MessageFormat(
                "\"NewSessionTicket\": '{'\n" +
                "  \"ticket_lifetime\"      : \"{0}\",\n" +
                "  \"ticket_age_add\"       : \"{1}\",\n" +
                "  \"ticket_nonce\"         : \"{2}\",\n" +
                "  \"ticket\"               : '{'\n" +
                "{3}\n" +
                "  '}'" +
                "  \"extensions\"           : [\n" +
                "{4}\n" +
                "  ]\n" +
                "'}'",
                Locale.ENGLISH);

            HexDumpEncoder hexEncoder = new HexDumpEncoder();
            Object[] messageFields = {
                ticketLifetime,
                "<omitted>",    //ticketAgeAdd should not be logged
                Utilities.toHexString(ticketNonce),
                Utilities.indent(hexEncoder.encode(ticket), "    "),
                Utilities.indent(extensions.toString(), "    ")
            };

            return messageFormat.format(messageFields);
        }
    }

    private static SecretKey derivePreSharedKey(CipherSuite.HashAlg hashAlg,
            SecretKey resumptionMasterSecret, byte[] nonce) throws IOException {
        try {
            HKDF hkdf = new HKDF(hashAlg.name);
            byte[] hkdfInfo = SSLSecretDerivation.createHkdfInfo(
                    "tls13 resumption".getBytes(), nonce, hashAlg.hashLength);
            return hkdf.expand(resumptionMasterSecret, hkdfInfo,
                    hashAlg.hashLength, "TlsPreSharedKey");
        } catch  (GeneralSecurityException gse) {
            throw (SSLHandshakeException) new SSLHandshakeException(
                    "Could not derive PSK").initCause(gse);
        }
    }

    private static final
            class NewSessionTicketKickstartProducer implements SSLProducer {
        // Prevent instantiation of this class.
        private NewSessionTicketKickstartProducer() {
            // blank
        }

        @Override
        public byte[] produce(ConnectionContext context) throws IOException {
            // The producing happens in server side only.
            ServerHandshakeContext shc = (ServerHandshakeContext)context;

            // Is this session resumable?
            if (!shc.handshakeSession.isRejoinable()) {
                return null;
            }

            // What's the requested PSK key exchange modes?
            //
            // Note that currently, the NewSessionTicket post-handshake is
            // produced and delivered only in the current handshake context
            // if required.
            PskKeyExchangeModesSpec pkemSpec =
                    (PskKeyExchangeModesSpec)shc.handshakeExtensions.get(
                            SSLExtension.PSK_KEY_EXCHANGE_MODES);
            if (pkemSpec == null || !pkemSpec.contains(
                PskKeyExchangeModesExtension.PskKeyExchangeMode.PSK_DHE_KE)) {
                // Client doesn't support PSK with (EC)DHE key establishment.
                return null;
            }

            // get a new session ID
            SSLSessionContextImpl sessionCache = (SSLSessionContextImpl)
                shc.sslContext.engineGetServerSessionContext();
            SessionId newId = new SessionId(true,
                shc.sslContext.getSecureRandom());

            SecretKey resumptionMasterSecret =
                shc.handshakeSession.getResumptionMasterSecret();
            if (resumptionMasterSecret == null) {
                if (SSLLogger.isOn && SSLLogger.isOn("ssl,handshake")) {
                    SSLLogger.fine(
                        "Session has no resumption secret. No ticket sent.");
                }
                return null;
            }

            // construct the PSK and handshake message
            BigInteger nonce = shc.handshakeSession.incrTicketNonceCounter();
            byte[] nonceArr = nonce.toByteArray();
            SecretKey psk = derivePreSharedKey(
                    shc.negotiatedCipherSuite.hashAlg,
                    resumptionMasterSecret, nonceArr);

            int sessionTimeoutSeconds = sessionCache.getSessionTimeout();
            if (sessionTimeoutSeconds > MAX_TICKET_LIFETIME) {
                if (SSLLogger.isOn && SSLLogger.isOn("ssl,handshake")) {
                    SSLLogger.fine(
                        "Session timeout is too long. No ticket sent.");
                }
                return null;
            }

            NewSessionTicketMessage nstm;

            SSLSessionImpl sessionCopy =
                    new SSLSessionImpl(shc.handshakeSession, newId);
            sessionCopy.setPreSharedKey(psk);
            sessionCopy.setPskIdentity(newId.getId());

            if (shc.statelessResumption) {
                try {
                    nstm = new T13NewSessionTicketMessage(shc,
                            sessionTimeoutSeconds, shc.sslContext.getSecureRandom(),
                            nonceArr, new SessionTicketSpec().encrypt(shc, sessionCopy));
                    if (SSLLogger.isOn && SSLLogger.isOn("ssl,handshake")) {
                        SSLLogger.fine(
                                "Produced NewSessionTicket stateless " +
                                        "handshake message", nstm);
                    }
                } catch (Exception e) {
                    // Error with NST ticket, abort NST
                    shc.conContext.fatal(Alert.UNEXPECTED_MESSAGE, e);
                    return null;
                }
            } else {
                nstm = new T13NewSessionTicketMessage(shc, sessionTimeoutSeconds,
                        shc.sslContext.getSecureRandom(), nonceArr,
                        newId.getId());
                if (SSLLogger.isOn && SSLLogger.isOn("ssl,handshake")) {
                    SSLLogger.fine(
                            "Produced NewSessionTicket handshake message",
                            nstm);
                }

                // create and cache the new session
                // The new session must be a child of the existing session so
                // they will be invalidated together, etc.
                shc.handshakeSession.addChild(sessionCopy);
                sessionCopy.setTicketAgeAdd(nstm.getTicketAgeAdd());
                sessionCache.put(sessionCopy);
            }
            // Output the handshake message.
            nstm.write(shc.handshakeOutput);
            shc.handshakeOutput.flush();

            // The message has been delivered.
            return null;
        }
    }

    /**
     * The "NewSessionTicket" handshake message producer for RFC 5077
     */
    private static final class T12NewSessionTicketProducer
            implements HandshakeProducer {

        // Prevent instantiation of this class.
        private T12NewSessionTicketProducer() {
            // blank
        }

        @Override
        public byte[] produce(ConnectionContext context,
                HandshakeMessage message) throws IOException {

            ServerHandshakeContext shc = (ServerHandshakeContext)context;

            // Is this session resumable?
            if (!shc.handshakeSession.isRejoinable()) {
                return null;
            }

            // get a new session ID
            SessionId newId = shc.handshakeSession.getSessionId();

            SSLSessionContextImpl sessionCache = (SSLSessionContextImpl)
                    shc.sslContext.engineGetServerSessionContext();
            int sessionTimeoutSeconds = sessionCache.getSessionTimeout();
            if (sessionTimeoutSeconds > MAX_TICKET_LIFETIME) {
                if (SSLLogger.isOn && SSLLogger.isOn("ssl,handshake")) {
                    SSLLogger.fine(
                        "Session timeout is too long. No ticket sent.");
                }
                return null;
            }

            NewSessionTicketMessage nstm;

            SSLSessionImpl sessionCopy =
                    new SSLSessionImpl(shc.handshakeSession, newId);
            sessionCopy.setPskIdentity(newId.getId());

            try {
                nstm = new T12NewSessionTicketMessage(shc, sessionTimeoutSeconds,
                        new SessionTicketSpec().encrypt(shc, sessionCopy));
                if (SSLLogger.isOn && SSLLogger.isOn("ssl,handshake")) {
                    SSLLogger.fine(
                            "Produced NewSessionTicket stateless handshake message", nstm);
                }
            } catch (Exception e) {
                // Abort on error with NST ticket
                shc.conContext.fatal(Alert.UNEXPECTED_MESSAGE, e);
                return null;
            }

            // Output the handshake message.
            nstm.write(shc.handshakeOutput);
            shc.handshakeOutput.flush();

            // The message has been delivered.
            return null;
        }
    }

    private static final
    class T13NewSessionTicketConsumer implements SSLConsumer {
        // Prevent instantiation of this class.
        private T13NewSessionTicketConsumer() {
            // blank
        }

        @Override
        public void consume(ConnectionContext context,
                ByteBuffer message) throws IOException {

            // Note: Although the resumption master secret depends on the
            // client's second flight, servers which do not request client
            // authentication MAY compute the remainder of the transcript
            // independently and then send a NewSessionTicket immediately
            // upon sending its Finished rather than waiting for the client
            // Finished.
            //
            // The consuming happens in client side only and is received after
            // the server's Finished message with PostHandshakeContext.

            HandshakeContext hc = (HandshakeContext)context;
            NewSessionTicketMessage nstm =
                    new T13NewSessionTicketMessage(hc, message);
            if (SSLLogger.isOn && SSLLogger.isOn("ssl,handshake")) {
                SSLLogger.fine(
                "Consuming NewSessionTicket message", nstm);
            }

            // discard tickets with timeout 0
            if (nstm.ticketLifetime <= 0 ||
                nstm.ticketLifetime > MAX_TICKET_LIFETIME) {
                if (SSLLogger.isOn && SSLLogger.isOn("ssl,handshake")) {
                    SSLLogger.fine(
                    "Discarding NewSessionTicket with lifetime "
                        + nstm.ticketLifetime, nstm);
                }
                return;
            }

            SSLSessionContextImpl sessionCache = (SSLSessionContextImpl)
                hc.sslContext.engineGetClientSessionContext();

            if (sessionCache.getSessionTimeout() > MAX_TICKET_LIFETIME) {
                if (SSLLogger.isOn && SSLLogger.isOn("ssl,handshake")) {
                    SSLLogger.fine(
                    "Session cache lifetime is too long. Discarding ticket.");
                }
                return;
            }

            SSLSessionImpl sessionToSave = hc.conContext.conSession;
            SecretKey psk = null;
            if (hc.negotiatedProtocol.useTLS13PlusSpec()) {
                SecretKey resumptionMasterSecret =
                        sessionToSave.getResumptionMasterSecret();
                if (resumptionMasterSecret == null) {
                    if (SSLLogger.isOn && SSLLogger.isOn("ssl,handshake")) {
                        SSLLogger.fine(
                                "Session has no resumption master secret." +
                                        " Ignoring ticket.");
                    }
                    return;
                }

                // derive the PSK
                psk = derivePreSharedKey(
                        sessionToSave.getSuite().hashAlg,
                        resumptionMasterSecret, nstm.getTicketNonce());
            }

            // create and cache the new session
            // The new session must be a child of the existing session so
            // they will be invalidated together, etc.
            SessionId newId =
                    new SessionId(true, hc.sslContext.getSecureRandom());
            SSLSessionImpl sessionCopy = new SSLSessionImpl(sessionToSave,
                    newId);
            sessionToSave.addChild(sessionCopy);
            sessionCopy.setPreSharedKey(psk);
            sessionCopy.setTicketAgeAdd(nstm.getTicketAgeAdd());
            sessionCopy.setPskIdentity(nstm.ticket);
            sessionCache.put(sessionCopy);

            // clean handshake context
            if (hc.negotiatedProtocol.useTLS13PlusSpec()) {
                hc.conContext.finishPostHandshake();
            }
        }
    }

    private static final
    class T12NewSessionTicketConsumer implements SSLConsumer {
        // Prevent instantiation of this class.
        private T12NewSessionTicketConsumer() {
            // blank
        }

        @Override
        public void consume(ConnectionContext context,
                ByteBuffer message) throws IOException {

            HandshakeContext hc = (HandshakeContext)context;
            hc.handshakeConsumers.remove(NEW_SESSION_TICKET.id);

            NewSessionTicketMessage nstm = new T12NewSessionTicketMessage(hc,
                    message);
            if (nstm.ticket.length == 0) {
                if (SSLLogger.isOn && SSLLogger.isOn("ssl,handshake")) {
                    SSLLogger.fine("NewSessionTicket ticket was empty");
                }
                return;
            }

            // discard tickets with timeout 0
            if (nstm.ticketLifetime <= 0 ||
                nstm.ticketLifetime > MAX_TICKET_LIFETIME) {
                if (SSLLogger.isOn && SSLLogger.isOn("ssl,handshake")) {
                    SSLLogger.fine(
                    "Discarding NewSessionTicket with lifetime "
                        + nstm.ticketLifetime, nstm);
                }
                return;
            }

            SSLSessionContextImpl sessionCache = (SSLSessionContextImpl)
                    hc.sslContext.engineGetClientSessionContext();

            if (sessionCache.getSessionTimeout() > MAX_TICKET_LIFETIME) {
                if (SSLLogger.isOn && SSLLogger.isOn("ssl,handshake")) {
                    SSLLogger.fine(
                    "Session cache lifetime is too long. Discarding ticket.");
                }
                return;
            }

            hc.handshakeSession.setPskIdentity(nstm.ticket);
            if (SSLLogger.isOn && SSLLogger.isOn("ssl,handshake")) {
                SSLLogger.fine("Consuming NewSessionTicket\n" +
                        nstm.toString());
            }
        }
    }
}

