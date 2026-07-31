// Direct test of FtpBackend's directory-listing parsers, against
// realistic sample lines modeled on real server output formats — not
// invented syntax. This sandbox has no reachable FTP server, so this is
// the only way to verify the single highest-risk part of the FTP/FTPS
// feature: FTP's LIST output format is famously NOT standardized across
// server implementations (confirmed directly against libcurl's own
// documentation before this feature's architecture was even decided —
// libcurl doesn't parse it either, callers still have to), which is
// exactly why MLSD (a real, RFC 3659-standardized alternative) is tried
// first and LIST is only a best-effort fallback.
//
// What this test does NOT cover: the actual network protocol (control
// connection command/reply cycling, PASV parsing, the AUTH TLS
// handshake for FTPS) — none of that is reachable without a live
// server. The parsers tested here are pure functions with no I/O,
// deliberately factored out and made public specifically so they could
// be verified this way — same reasoning FilePaneWidget::parentOfPath()
// was made public for.
#include <QCoreApplication>
#include <QDebug>
#include "backends/FtpBackend.h"
#include "backends/RemoteEntry.h"

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);   // needed for QRegularExpression's use of Qt's global data structures

    bool allPass = true;
    auto check = [&](const QString &label, bool condition) {
        qDebug() << (condition ? "[PASS]" : "[FAIL]") << label;
        if (!condition) allPass = false;
    };

    // ===================================================================
    // MLSD — standardized, should parse exactly
    // ===================================================================
    {
        RemoteEntry entry;
        const bool ok = FtpBackend::parseMlsdLine(
            "Type=file;Size=1234;Modify=20240115103045; report.pdf", &entry);
        check("MLSD file: parsed successfully", ok);
        check("MLSD file: correct name", entry.name == "report.pdf");
        check("MLSD file: isDir is false", !entry.isDir);
        check("MLSD file: correct size", entry.size == 1234);
        check("MLSD file: correct modified date",
              entry.modified == QDateTime(QDate(2024, 1, 15), QTime(10, 30, 45), Qt::UTC));
    }
    {
        RemoteEntry entry;
        const bool ok = FtpBackend::parseMlsdLine("Type=dir;Modify=20240115103045; subfolder", &entry);
        check("MLSD directory: parsed successfully", ok);
        check("MLSD directory: correct name", entry.name == "subfolder");
        check("MLSD directory: isDir is true", entry.isDir);
    }
    {
        RemoteEntry entry;
        const bool cdirOk = FtpBackend::parseMlsdLine("Type=cdir;Modify=20240115103045; .", &entry);
        check("MLSD: cdir (the '.' equivalent) is filtered, not returned as a real entry", !cdirOk);
        const bool pdirOk = FtpBackend::parseMlsdLine("Type=pdir;Modify=20240115103045; ..", &entry);
        check("MLSD: pdir (the '..' equivalent) is filtered, not returned as a real entry", !pdirOk);
    }
    {
        // A filename containing a space — the real reason MLSD splits on
        // the LAST semicolon rather than counting fields.
        RemoteEntry entry;
        const bool ok = FtpBackend::parseMlsdLine(
            "Type=file;Size=500;Modify=20240115103045; my vacation photo.jpg", &entry);
        check("MLSD: filename containing a space parsed correctly", ok && entry.name == "my vacation photo.jpg");
    }

    // ===================================================================
    // LIST — Unix ls -l style, the overwhelmingly common real-world format
    // ===================================================================
    {
        RemoteEntry entry;
        const bool ok = FtpBackend::parseListLine(
            "-rw-r--r--    1 ftpuser  ftpgroup     4096 Jan 15 10:30 report.pdf", &entry);
        check("LIST (Unix, recent file with time): parsed successfully", ok);
        check("LIST (Unix, recent file): correct name", entry.name == "report.pdf");
        check("LIST (Unix, recent file): isDir is false", !entry.isDir);
        check("LIST (Unix, recent file): correct size", entry.size == 4096);
    }
    {
        RemoteEntry entry;
        // Older file: the date field shows a YEAR instead of a time —
        // the real-world formatting quirk that makes naive parsing so
        // easy to get subtly wrong.
        const bool ok = FtpBackend::parseListLine(
            "-rw-r--r--    1 ftpuser  ftpgroup     8192 Mar  3  2019 old_archive.zip", &entry);
        check("LIST (Unix, old file with year instead of time): parsed successfully", ok);
        check("LIST (Unix, old file): correct name", entry.name == "old_archive.zip");
        check("LIST (Unix, old file): correct size", entry.size == 8192);
    }
    {
        RemoteEntry entry;
        const bool ok = FtpBackend::parseListLine(
            "drwxr-xr-x    2 ftpuser  ftpgroup     4096 Jan 15 10:30 subfolder", &entry);
        check("LIST (Unix, directory): parsed successfully", ok);
        check("LIST (Unix, directory): isDir is true", entry.isDir);
        check("LIST (Unix, directory): correct name", entry.name == "subfolder");
    }
    {
        RemoteEntry entry;
        const bool ok = FtpBackend::parseListLine(
            "lrwxrwxrwx    1 ftpuser  ftpgroup       11 Jan 15 10:30 shortcut -> report.pdf", &entry);
        check("LIST (Unix, symlink): parsed successfully", ok);
        check("LIST (Unix, symlink): isSymlink is true", entry.isSymlink);
        check("LIST (Unix, symlink): the '-> target' part is stripped from the name",
              entry.name == "shortcut");
    }
    {
        RemoteEntry entry;
        // A filename containing a space, still handled correctly since
        // the regex captures "everything after the date" as the name.
        const bool ok = FtpBackend::parseListLine(
            "-rw-r--r--    1 ftpuser  ftpgroup     2048 Jan 15 10:30 my vacation photo.jpg", &entry);
        check("LIST (Unix): filename containing a space parsed correctly",
              ok && entry.name == "my vacation photo.jpg");
    }

    // ===================================================================
    // LIST — DOS/Windows IIS style, the other real-world format actually
    // seen in the wild (older IIS FTP servers, some embedded devices)
    // ===================================================================
    {
        RemoteEntry entry;
        const bool ok = FtpBackend::parseListLine("01-15-24  10:30AM       <DIR>          subfolder", &entry);
        check("LIST (DOS, directory): parsed successfully", ok);
        check("LIST (DOS, directory): isDir is true", entry.isDir);
        check("LIST (DOS, directory): correct name", entry.name == "subfolder");
    }
    {
        RemoteEntry entry;
        const bool ok = FtpBackend::parseListLine("01-15-24  10:30AM             4096 report.pdf", &entry);
        check("LIST (DOS, file): parsed successfully", ok);
        check("LIST (DOS, file): isDir is false", !entry.isDir);
        check("LIST (DOS, file): correct size", entry.size == 4096);
        check("LIST (DOS, file): correct name", entry.name == "report.pdf");
    }

    // ===================================================================
    // Lines that should NOT parse as entries — the caller (listDirectoryInternal)
    // skips these rather than treating them as a listing failure or
    // fabricating a garbage entry
    // ===================================================================
    {
        RemoteEntry entry;
        check("LIST: a leading 'total N' summary line does not parse as an entry",
              !FtpBackend::parseListLine("total 24", &entry));
        check("LIST: an empty line does not parse as an entry",
              !FtpBackend::parseListLine("", &entry));
        check("MLSD: an empty line does not parse as an entry",
              !FtpBackend::parseMlsdLine("", &entry));
        check("MLSD: a line with no semicolon at all does not parse as an entry",
              !FtpBackend::parseMlsdLine("not a valid mlsd line", &entry));
    }

    qDebug() << (allPass ? "[test] ALL PASS" : "[test] AT LEAST ONE FAILURE");
    return allPass ? 0 : 1;
}
