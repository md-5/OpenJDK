/**
 * @test
 * @bug 7013521
 * @summary AIFF/AU/WAVE writers close input audio stream
 * @author Alex Menkov
 */

import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import javax.sound.sampled.AudioFileFormat;
import javax.sound.sampled.AudioFormat;
import javax.sound.sampled.AudioInputStream;
import javax.sound.sampled.AudioSystem;


public class WriterCloseInput {

    final static AudioFormat audioFormat = new AudioFormat(44100f, 16, 2, true, true);
    //final static AudioFormat audioFormat = new AudioFormat(AudioFormat.Encoding.ULAW, 44100f, 8, 2, 2, 44100f, true);
    final static int frameLength = 44100 * 2; // 2 seconds
    final static byte[] dataBuffer
            = new byte[frameLength * (audioFormat.getSampleSizeInBits()/8)
                       * audioFormat.getChannels()];

    static int testTotal = 0;
    static int testFailed = 0;

    public static void main(String[] args) throws Exception {
        test(AudioFileFormat.Type.AIFF);
        test(AudioFileFormat.Type.AU);
        test(AudioFileFormat.Type.WAVE);

        if (testFailed == 0) {
            out("All tests passed.");
        } else {
            out("" + testFailed + " of " + testTotal + " tests FAILED.");
            System.out.flush();
            throw new RuntimeException("Test FAILED.");
        }
    }

    static void test(AudioFileFormat.Type fileType) {
        test(fileType, frameLength);
        test(fileType, AudioSystem.NOT_SPECIFIED);
    }

    static void test(AudioFileFormat.Type fileType, int length) {
        test(fileType, length, false);
        test(fileType, length, true);
    }

    static void test(AudioFileFormat.Type fileType, int length, boolean isFile) {
        testTotal++;
        out("Testing fileType: " + fileType
                + ", frameLength: " + (length >= 0 ? length : "unspecified")
                + ", output: " + (isFile ? "File" : "OutputStream"));
        AudioInputStream inStream = new ThrowAfterCloseStream(
                new ByteArrayInputStream(dataBuffer), audioFormat, length);

        AudioSystem.isFileTypeSupported(fileType, inStream);

        try {
            if (isFile) {
                File f = File.createTempFile("WriterCloseInput" + testTotal, "tmp");
                AudioSystem.write(inStream, fileType, f);
                f.delete();
            } else {
                OutputStream outStream = new NullOutputStream();
                AudioSystem.write(inStream, fileType, outStream);
            }
        } catch (Exception ex) {
            // this is not failure
            out("SKIPPED (AudioSystem.write exception): " + ex.getMessage());
            //out(ex);
            inStream = null;
        }

        if (inStream != null) {
            try {
                // test if the stream is closed
                inStream.available();
                out("PASSED");
            } catch (IOException ex) {
                testFailed++;
                out("FAILED: " + ex.getMessage());
                //out(ex);
            }
        }
        out("");
    }

    static class ThrowAfterCloseStream extends AudioInputStream {
        private boolean closed = false;
        public ThrowAfterCloseStream(InputStream in, AudioFormat format, long length) {
            super(in, format, length);
        }
        @Override
        public void close() {
            closed = true;
        }
        @Override
        public int available() throws IOException {
            if (closed) {
                throw new IOException("The stream has been closed");
            }
            return 1;
        }
    }

    static class NullOutputStream extends OutputStream {
        @Override
        public void write(int b) throws IOException {
            // nop
        }
    }

    static void out(String s) {
        System.out.println(s);
    }

    static void out(Exception ex) {
        ex.printStackTrace(System.out);
    }
}
