#pragma once

/**
 * SerialPort — lightweight cross-platform serial port abstraction.
 *
 * Written for Patchy. No external dependencies, no licence constraints.
 * Platform branching: POSIX (macOS / Linux) and Win32 (Windows).
 *
 * API surface deliberately minimal — covers exactly what the
 * Enttec DMX USB Pro protocol needs:
 *   open / close / write / read / listPorts
 *
 * Usage:
 *   SerialPort port;
 *   if (port.open("/dev/cu.usbserial-XXXX", 57600, 8, 'N', 2)) {
 *       uint8_t buf[] = { 0x7E, 0x06, ... };
 *       port.write(buf, sizeof(buf));
 *       port.close();
 *   }
 *   auto ports = SerialPort::listPorts();  // → vector of device path strings
 */

#include <string>
#include <vector>
#include <cstdint>

// ── Platform includes ─────────────────────────────────────────────────────────
#if defined(_WIN32) || defined(_WIN64)
  #define SERIALPORT_WINDOWS
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#else
  #define SERIALPORT_POSIX
  #include <fcntl.h>
  #include <termios.h>
  #include <unistd.h>
  #include <sys/ioctl.h>
  #include <dirent.h>
  #include <cstring>
  #include <cerrno>
#endif

class SerialPort
{
public:
    SerialPort()  = default;
    ~SerialPort() { close(); }

    // Non-copyable
    SerialPort (const SerialPort&) = delete;
    SerialPort& operator= (const SerialPort&) = delete;

    // ── open ─────────────────────────────────────────────────────────────────
    /**
     * Open a serial port.
     *
     * @param device   Port path — "/dev/cu.usbserial-XXXX" on macOS,
     *                 "/dev/ttyUSB0" on Linux, "COM3" on Windows.
     * @param baudRate Baud rate (e.g. 57600 for Enttec Pro).
     * @param dataBits 8
     * @param parity   'N' = none, 'E' = even, 'O' = odd
     * @param stopBits 1 or 2
     * @return true on success.
     */
    bool open (const std::string& device,
               unsigned int       baudRate = 57600,
               int                dataBits = 8,
               char               parity   = 'N',
               int                stopBits = 2)
    {
        close();

#if defined(SERIALPORT_POSIX)
        fd_ = ::open (device.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd_ < 0) return false;

        // Switch to blocking mode
        int flags = fcntl (fd_, F_GETFL, 0);
        fcntl (fd_, F_SETFL, flags & ~O_NONBLOCK);

        struct termios tty {};
        if (tcgetattr (fd_, &tty) != 0) { ::close (fd_); fd_ = -1; return false; }

        // Baud rate
        speed_t speed = toBaudRate (baudRate);
        cfsetispeed (&tty, speed);
        cfsetospeed (&tty, speed);

        // Raw mode
        cfmakeraw (&tty);

        // Data bits
        tty.c_cflag &= ~(tcflag_t)CSIZE;
        tty.c_cflag |= (tcflag_t)((dataBits == 7) ? CS7 : CS8);

        // Stop bits
        if (stopBits == 2) tty.c_cflag |=  (tcflag_t)CSTOPB;
        else               tty.c_cflag &= ~(tcflag_t)CSTOPB;

        // Parity
        if (parity == 'E')      { tty.c_cflag |= (tcflag_t)PARENB; tty.c_cflag &= ~(tcflag_t)PARODD; }
        else if (parity == 'O') { tty.c_cflag |= (tcflag_t)PARENB; tty.c_cflag |=  (tcflag_t)PARODD; }
        else                    { tty.c_cflag &= ~(tcflag_t)PARENB; }

        // Enable receiver, local mode
        tty.c_cflag |= (tcflag_t)(CLOCAL | CREAD);

        // Blocking read — return after 1 char or 100ms timeout
        tty.c_cc[VMIN]  = 0;
        tty.c_cc[VTIME] = 1;   // 100ms units

        if (tcsetattr (fd_, TCSANOW, &tty) != 0) { ::close (fd_); fd_ = -1; return false; }

        tcflush (fd_, TCIOFLUSH);
        return true;

#elif defined(SERIALPORT_WINDOWS)
        std::string path = "\\\\.\\" + device;
        handle_ = CreateFileA (path.c_str(),
                               GENERIC_READ | GENERIC_WRITE,
                               0, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) return false;

        DCB dcb {};
        dcb.DCBlength = sizeof (DCB);
        if (! GetCommState (handle_, &dcb)) { CloseHandle (handle_); handle_ = INVALID_HANDLE_VALUE; return false; }

        dcb.BaudRate = baudRate;
        dcb.ByteSize = (BYTE) dataBits;
        dcb.StopBits = (stopBits == 2) ? TWOSTOPBITS : ONESTOPBIT;
        dcb.Parity   = (parity == 'E') ? EVENPARITY
                      : (parity == 'O') ? ODDPARITY : NOPARITY;
        dcb.fParity  = (parity != 'N') ? TRUE : FALSE;

        if (! SetCommState (handle_, &dcb)) { CloseHandle (handle_); handle_ = INVALID_HANDLE_VALUE; return false; }

        COMMTIMEOUTS timeouts {};
        timeouts.ReadIntervalTimeout         = 10;
        timeouts.ReadTotalTimeoutMultiplier  = 1;
        timeouts.ReadTotalTimeoutConstant    = 100;
        timeouts.WriteTotalTimeoutMultiplier = 1;
        timeouts.WriteTotalTimeoutConstant   = 100;
        SetCommTimeouts (handle_, &timeouts);

        PurgeComm (handle_, PURGE_RXCLEAR | PURGE_TXCLEAR);
        return true;
#endif
    }

    // ── close ────────────────────────────────────────────────────────────────
    void close()
    {
#if defined(SERIALPORT_POSIX)
        if (fd_ >= 0) { ::close (fd_); fd_ = -1; }
#elif defined(SERIALPORT_WINDOWS)
        if (handle_ != INVALID_HANDLE_VALUE) { CloseHandle (handle_); handle_ = INVALID_HANDLE_VALUE; }
#endif
    }

    // Takes ownership of another SerialPort's own already-open connection,
    // leaving that instance closed (never touching the same underlying
    // handle twice) — added 2026-09-01 to fix a real graph-wide
    // sluggishness bug: ProcessingGraph::rebuild() destroys and recreates
    // every node instance on every single graph edit, anywhere, not just
    // ones involving this specific device — meaning a plain "skip
    // reopening if unchanged" check inside configure() itself (an earlier,
    // structurally broken attempt at this same fix) could never work, since
    // a freshly-constructed node's own member variables (including this
    // whole SerialPort, and the devicePath it would have been compared
    // against) are always at their own fresh defaults, never carrying over
    // from the instance that came before. This lets the caller — the
    // node's own equivalent transfer method, called from
    // PatchyProcessor::rebuildProcessingGraph()'s own existing "transfer
    // state across rebuild" mechanism (see DmxConsoleNode's own
    // transferLastSent for the established precedent) — genuinely reuse
    // an already-open, already-negotiated connection instead of closing
    // and reopening it from scratch on every unrelated graph edit.
    void transferFrom (SerialPort& other)
    {
        close();
#if defined(SERIALPORT_POSIX)
        fd_ = other.fd_;
        other.fd_ = -1;
#elif defined(SERIALPORT_WINDOWS)
        handle_ = other.handle_;
        other.handle_ = INVALID_HANDLE_VALUE;
#endif
    }

    bool isOpen() const
    {
#if defined(SERIALPORT_POSIX)
        return fd_ >= 0;
#elif defined(SERIALPORT_WINDOWS)
        return handle_ != INVALID_HANDLE_VALUE;
#else
        return false;
#endif
    }

    // ── write ────────────────────────────────────────────────────────────────
    /** Write bytes. Returns number of bytes written, or -1 on error. */
    int write (const void* buf, int len)
    {
        if (! isOpen() || buf == nullptr || len <= 0) return -1;
#if defined(SERIALPORT_POSIX)
        return (int) ::write (fd_, buf, (size_t) len);
#elif defined(SERIALPORT_WINDOWS)
        DWORD written = 0;
        return WriteFile (handle_, buf, (DWORD) len, &written, nullptr) ? (int) written : -1;
#endif
    }

    // ── read ─────────────────────────────────────────────────────────────────
    /** Read up to maxLen bytes with a timeout. Returns bytes read, 0 on timeout, -1 on error. */
    int read (void* buf, int maxLen, int timeoutMs = 100)
    {
        if (! isOpen() || buf == nullptr || maxLen <= 0) return -1;
#if defined(SERIALPORT_POSIX)
        // Use select() for timeout
        fd_set fds;
        FD_ZERO (&fds);
        FD_SET (fd_, &fds);
        struct timeval tv { timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
        int ready = select (fd_ + 1, &fds, nullptr, nullptr, &tv);
        if (ready <= 0) return ready;   // 0 = timeout, -1 = error
        return (int) ::read (fd_, buf, (size_t) maxLen);
#elif defined(SERIALPORT_WINDOWS)
        DWORD bytesRead = 0;
        return ReadFile (handle_, buf, (DWORD) maxLen, &bytesRead, nullptr) ? (int) bytesRead : -1;
#endif
    }

    // ── listPorts ────────────────────────────────────────────────────────────
    /**
     * Enumerate available serial ports.
     * On macOS: scans /dev/ for cu.usbserial* and cu.usbmodem*
     * On Linux: scans /dev/ for ttyUSB* and ttyACM*
     * On Windows: probes COM1–COM256
     */
    static std::vector<std::string> listPorts()
    {
        std::vector<std::string> result;

#if defined(SERIALPORT_POSIX)
        DIR* dir = opendir ("/dev");
        if (dir == nullptr) return result;

        struct dirent* entry;
        while ((entry = readdir (dir)) != nullptr)
        {
            std::string name = entry->d_name;
#if defined(__APPLE__)
            if (name.find ("cu.usbserial") == 0 ||
                name.find ("cu.usbmodem")  == 0)
                result.push_back ("/dev/" + name);
#else
            if (name.find ("ttyUSB") == 0 ||
                name.find ("ttyACM") == 0)
                result.push_back ("/dev/" + name);
#endif
        }
        closedir (dir);

        // Sort for consistent ordering
        std::sort (result.begin(), result.end());

#elif defined(SERIALPORT_WINDOWS)
        for (int i = 1; i <= 256; ++i)
        {
            std::string port = "COM" + std::to_string (i);
            std::string path = "\\\\.\\" + port;
            HANDLE h = CreateFileA (path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                    0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h != INVALID_HANDLE_VALUE)
            {
                result.push_back (port);
                CloseHandle (h);
            }
        }
#endif
        return result;
    }

private:
#if defined(SERIALPORT_POSIX)
    int fd_ = -1;

    static speed_t toBaudRate (unsigned int baud)
    {
        switch (baud)
        {
            case 9600:   return B9600;
            case 19200:  return B19200;
            case 38400:  return B38400;
            case 57600:  return B57600;
            case 115200: return B115200;
            case 230400: return B230400;
            default:     return B57600;
        }
    }

#elif defined(SERIALPORT_WINDOWS)
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#endif
};
