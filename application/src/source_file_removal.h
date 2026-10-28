#pragma once
#include "index.h"
#include <QString>

// Resolve an indexed item to an existing original inside its library root.
// The library root itself and paths that escape through links are never valid targets.
QString validatedOriginalPath(const Library &library, const QString &relativePath, bool folder,
                              QString *error = nullptr);

// Move one original item to the operating system trash. Never falls back to permanent deletion.
// Call off the UI thread for large folders; no catalog change is made by this function.
bool moveOriginalToTrash(const Library &library, const QString &relativePath, bool folder,
                         const QList<Library> &allLibraries, QString *error = nullptr);

// Entire-library removal requires an explicit UI confirmation. Refuse volume roots and
// any directory containing another active library or shared cache.
bool moveLibraryRootToTrash(const Library &library, const QList<Library> &allLibraries,
                            QString *error = nullptr);
