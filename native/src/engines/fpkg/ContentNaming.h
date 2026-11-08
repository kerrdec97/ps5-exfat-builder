#pragma once

// Content IDs, content versions and package file names.
//
//  * Content ID: XXYYYY-XXXXYYYYY_NN-<16 A-Z/0-9>. The project accepts any two
//    digits after the underscore; "_00" is the usual value and anything else
//    is only warned about.
//  * Content version: NN.NNN.NNN. The package-name component is the 2-digit
//    major followed by the minor with at least two digits ("01.000.000" ->
//    "0100", "01.512.000" -> "01512"). Both the A and the V part of the file
//    name come from contentVersion.

#include <QString>

#include <optional>

namespace fw::fpkg {

bool isValidContentId(const QString& id);
bool hasStandardContentIdSuffix(const QString& id); // "_00"
QString titleIdOf(const QString& contentId);        // "PPSA01234"

struct ContentVersion {
    int major = 1;
    int minor = 0;
    int patch = 0;

    static std::optional<ContentVersion> parse(const QString& text); // strict NN.NNN.NNN
    QString text() const;                 // "01.000.000"
    QString packageComponent() const;     // "0100"
    // Next update version: middle group + 1, patch reset; "01.999.x" -> "02.000.000".
    ContentVersion next() const;
    // Backport updates (ppr-pkg Companion rule): last group + 1 with carry,
    // "01.000.999" -> "01.001.000"; nullopt past "99.999.999".
    std::optional<ContentVersion> nextPatch() const;
    int compare(const ContentVersion& o) const; // -1 / 0 / 1
    bool operator==(const ContentVersion& o) const { return compare(o) == 0; }
};

// "<contentId>-A<vv>-V<vv>" (no extension).
QString packageStem(const QString& contentId, const ContentVersion& version);
QString fullPackageName(const QString& stem);   // stem + ".pkg"
QString updatePackageName(const QString& stem); // "UPDATE_" + stem + ".pkg"
QString companionName(const QString& stem);     // stem + ".remastered.pkg"

// "name.pkg" -> "name (n).pkg"; "name.remastered.pkg" -> "name (n).remastered.pkg".
QString numberedName(const QString& fileName, int n);

} // namespace fw::fpkg
