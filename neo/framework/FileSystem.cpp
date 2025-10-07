/*
===========================================================================

Doom 3 GPL Source Code
Copyright (C) 1999-2011 id Software LLC, a ZeniMax Media company.

This file is part of the Doom 3 GPL Source Code ("Doom 3 Source Code").

Doom 3 Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/

#ifdef WIN32
	#include <io.h>	// for _read
#else
	#include <sys/types.h>
	#include <sys/stat.h>
	#include <unistd.h>
#endif

#include "sys/platform.h"

#ifdef ID_ENABLE_CURL
	#include <curl/curl.h>
#endif

#include "idlib/hashing/MD4.h"
#include "framework/Licensee.h"
#include "framework/Unzip.h"
#include "framework/EventLoop.h"
#include "framework/DeclEntityDef.h"
#include "framework/DeclManager.h"
#include "framework/CVarSystem.h"
#include "framework/Common.h"
#include "framework/FileSystem.h"
#include "streamdb.rs.h"  // xAI: Rust FFI header for StreamDB integration

// xAI: StreamDB Integration - Replace pack_t with StreamDB wrapper
class idStreamDbPack : public pack_t {
private:
    rust::UniquePtr<ffi::StreamDb> db;
    uint32 checksum;
    bool addon;
    int pureStatus;

public:
    idStreamDbPack(const char* osPath, uint32 chksum);
    ~idStreamDbPack();

    bool Contains(const char* relPath);
    idFile* GetFile(const char* relPath);
    idStrList ListFiles(const char* prefix, const char* ext = "");
    uint32 GetChecksum() const { return checksum; }
    void SetPureStatus(int status) { pureStatus = status; }
    int GetPureStatus() const { return pureStatus; }
    void SetAddon(bool isAddon) { addon = isAddon; }
    bool IsAddon() const { return addon; }
};

idStreamDbPack::idStreamDbPack(const char* osPath, uint32 chksum) : checksum(chksum), addon(false), pureStatus(PURE_NEVER) {
    rust::String rustPath(osPath);
    auto result = ffi::open_db(&rustPath, true /* compression */, false /* quick mode */);
    if (result.is_err()) {
        common->FatalError("Failed to open StreamDB: %s", osPath);
    }
    db = result.unwrap();
    pakFilename = osPath;
    referenced = false;
    // xAI: Migrate legacy .pk4 if exists
    idFile* zipFile = fileSystem->OpenExplicitFileRead(osPath);
    if (zipFile && idStr::Icmp(osPath + idStr::Length(osPath) - 4, ".pk4") == 0) {
        // Placeholder for ZIP extraction: unzip each entry and write to StreamDB
        // Example: for each entry { read data; db->write_document(entry.name, data); }
        fileSystem->CloseFile(zipFile);
    }
}

idStreamDbPack::~idStreamDbPack() {
    db->close_db();
}

bool idStreamDbPack::Contains(const char* relPath) {
    rust::String rustPath(relPath);
    auto results = db->search_paths(&rustPath);
    return results.is_ok() && !results.unwrap().empty();
}

idFile* idStreamDbPack::GetFile(const char* relPath) {
    rust::String rustPath(relPath);
    auto result = db->get(&rustPath);
    if (result.is_err()) {
        return nullptr;
    }
    auto vec = result.unwrap();
    idFile_Memory* file = new idFile_Memory(relPath, vec.data(), static_cast<int>(vec.size()));
    return file;
}

idStrList idStreamDbPack::ListFiles(const char* prefix, const char* ext) {
    rust::String rustPrefix(prefix);
    auto results = db->search_paths(&rustPrefix);
    idStrList list;
    if (results.is_ok()) {
        for (const auto& p : results.unwrap()) {
            rust::String str(p);
            if (idStr::Icmp(idStr(str.c_str()).Right(idStr::Length(ext)), ext) == 0) {
                list.Append(str.c_str());
            }
        }
    }
    return list;
}

/*
=============================================================================

DOOM FILESYSTEM

All of Doom's data access is through a hierarchical file system, but the contents of
the file system can be transparently merged from several sources.

A "relativePath" is a reference to game file data, which must include a terminating zero.
"..", "\\", and ":" are explicitly illegal in qpaths to prevent any references
outside the Doom directory system.

The "base path" is the path to the directory holding all the game directories and
usually the executable. It defaults to the current directory, but can be overridden
with "+set fs_basepath c:\doom" on the command line. The base path cannot be modified
at all after startup.

The "save path" is the path to the directory where game files will be saved. It defaults
to the base path, but can be overridden with a "+set fs_savepath c:\doom" on the
command line. Any files that are created during the game (demos, screenshots, etc.) will
be created relative to the save path.

The "cd path" is the path to an alternate hierarchy that will be searched if a file
is not located in the base path. A user can do a partial install that copies some
data to a base path created on their hard drive and leave the rest on the cd. It defaults
to the current directory, but it can be overridden with "+set fs_cdpath g:\doom" on the
command line.

The "dev path" is the path to an alternate hierarchy where the editors and tools used
during development (Radiant, AF editor, dmap, runAAS) will write files to. It defaults to
the cd path, but can be overridden with a "+set fs_devpath c:\doom" on the command line.

If a user runs the game directly from a CD, the base path would be on the CD. This
should still function correctly, but all file writes will fail (harmlessly).

The "base game" is the directory under the paths where data comes from by default, and
can be either "base" or "demo".

The "current game" may be the same as the base game, or it may be the name of another
directory under the paths that should be searched for files before looking in the base
game. The game directory is set with "+set fs_game myaddon" on the command line. This is
the basis for addons.

No other directories outside of the base game and current game will ever be referenced by
filesystem functions.

Because we will have updated executables freely available online, there is no point to
trying to restrict demo / oem versions of the game with code changes. Demo / oem versions
should be exactly the same executables as release versions, but with different data that
automatically restricts where game media can come from to prevent add-ons from working.

After the paths are initialized, Doom will look for the product.txt file. If not found
and verified, the game will run in restricted mode. In restricted mode, only files
contained in demo/pak0.pk4 will be available for loading, and only if the zip header is
verified to not have been modified. A single exception is made for DoomConfig.cfg. Files
can still be written out in restricted mode, so screenshots and demos are possible.

The checksums for .pk4 files are stored in the pak file header. They are MD4 sums of the
file with the PK4 header stripped, and the header with the checksum replaced with 0xFFFFFFFF.
The checksum is calculated before compression, so it should be the same on all platforms.

=============================================================================
*/

// Ensure we have a singleton
idFileSystemLocal fileSystemLocal;
idFileSystem *fileSystem = &fileSystemLocal;

/*
================
idFileSystemLocal::idFileSystemLocal
================
*/
idFileSystemLocal::idFileSystemLocal( void ) {
    searchPaths = NULL;
    addonPaks = NULL;
    gameFolder = "";
}

/*
================
idFileSystemLocal::Init
================
*/
void idFileSystemLocal::Init( void ) {
    cmdSystem->AddCommand( "dir", Dir_f, CMD_FL_SYSTEM, "lists a folder", idCmdSystem::ArgCompletion_FileName );
    cmdSystem->AddCommand( "dirtree", DirTree_f, CMD_FL_SYSTEM, "lists a folder recursively", idCmdSystem::ArgCompletion_FileName );
    cmdSystem->AddCommand( "path", Path_f, CMD_FL_SYSTEM, "lists search paths" );
    cmdSystem->AddCommand( "touchFile", TouchFile_f, CMD_FL_SYSTEM, "touches a file" );
    cmdSystem->AddCommand( "touchFileList", TouchFileList_f, CMD_FL_SYSTEM, "touches a list of files" );
    // xAI: Add StreamDB build command
    cmdSystem->AddCommand( "buildSdb", BuildSdb_f, CMD_FL_SYSTEM, "builds a .sdb from directory" );
}

/*
================
idFileSystemLocal::Startup
================
*/
void idFileSystemLocal::Startup( void ) {
    searchpath_t **search;
    int i;
    pack_t *pak;
    int addon_index;

    common->Printf( "----- Initializing File System -----\n" );

    if ( restartChecksums.Num() ) {
        common->Printf( "restarting in pure mode with %d pak files\n", restartChecksums.Num() );
    }
    if ( addonChecksums.Num() ) {
        common->Printf( "restarting filesystem with %d addon pak file(s) to include\n", addonChecksums.Num() );
    }

    SetupGameDirectories( BASE_GAMEDIR );

    // fs_game_base override
    if ( fs_game_base.GetString()[0] &&
         idStr::Icmp( fs_game_base.GetString(), BASE_GAMEDIR ) ) {
        SetupGameDirectories( fs_game_base.GetString() );
    }

    // fs_game override
    if ( fs_game.GetString()[0] &&
         idStr::Icmp( fs_game.GetString(), BASE_GAMEDIR ) &&
         idStr::Icmp( fs_game.GetString(), fs_game_base.GetString() ) ) {
        SetupGameDirectories( fs_game.GetString() );
    }

    // currently all addons are in the search list - deal with filtering out and dependencies now
    search = &searchPaths;
    while ( *search ) {
        if ( !(*search)->pack || !(*search)->pack->addon ) {
            search = &((*search)->next);
            continue;
        }
        pak = (*search)->pack;
        if ( fs_searchAddons.GetBool() ) {
            assert( !addonChecksums.Num() );
            pak->addon_search = true;
            search = &((*search)->next);
            continue;
        }
        addon_index = addonChecksums.FindIndex( pak->checksum );
        if ( addon_index >= 0 ) {
            assert( !pak->addon_search );
            pak->addon_search = true;
            addonChecksums.RemoveIndex( addon_index );
            FollowAddonDependencies( pak );
        }
        search = &((*search)->next);
    }

    // now scan to filter out addons not marked addon_search
    search = &searchPaths;
    while ( *search ) {
        if ( !(*search)->pack || !(*search)->pack->addon ) {
            search = &((*search)->next);
            continue;
        }
        assert( !(*search)->dir );
        pak = (*search)->pack;
        if ( pak->addon_search ) {
            common->Printf( "Addon pk4 %s with checksum 0x%x is on the search list\n",
                            pak->pakFilename.c_str(), pak->checksum );
            search = &((*search)->next);
        } else {
            searchpath_t *paksearch = *search;
            *search = (*search)->next;
            paksearch->next = addonPaks;
            addonPaks = paksearch;
            common->Printf( "Addon pk4 %s with checksum 0x%x is on addon list\n",
                            pak->pakFilename.c_str(), pak->checksum );
        }
    }

    assert( !addonChecksums.Num() );
    addonChecksums.Clear();
}

/*
================
idFileSystemLocal::Shutdown
================
*/
void idFileSystemLocal::Shutdown( bool reloading ) {
    searchpath_t *search;

    if ( !searchPaths ) {
        common->FatalError( "Filesystem shutdown called when not initialized\n" );
    }

    // free dir cache
    ClearDirCache();

    // free the base search paths
    while ( searchPaths ) {
        search = searchPaths;
        searchPaths = searchPaths->next;
        if ( search->pack ) {
            delete search->pack;
        }
        if ( search->dir ) {
            delete search->dir;
        }
        delete search;
    }

    // free the addon search paths
    while ( addonPaks ) {
        search = addonPaks;
        addonPaks = addonPaks->next;
        if ( search->pack ) {
            delete search->pack;
        }
        delete search;
    }

    gameFolder.Clear();
}

/*
================
idFileSystemLocal::AddGameDirectory
================
*/
void idFileSystemLocal::AddGameDirectory( const char *path, const char *dir ) {
    int i;
    searchpath_t *search;
    pack_t *pak;
    idStr pakfile;
    idStrList pakfiles;

    // check if the search path already exists
    for ( search = searchPaths; search; search = search->next ) {
        if ( !search->dir ) {
            continue;
        }
        if ( search->dir->path.Cmp( path ) == 0 && search->dir->gamedir.Cmp( dir ) == 0 ) {
            return;
        }
    }

    gameFolder = dir;

    // add the directory to the search path
    search = new searchpath_t;
    search->dir = new directory_t;
    search->pack = NULL;

    search->dir->path = path;
    search->dir->gamedir = dir;
    search->next = searchPaths;
    searchPaths = search;

    // find all pak files in this directory
    pakfile = BuildOSPath( path, dir, "" );
    pakfile[ pakfile.Length() - 1 ] = 0; // strip the trailing slash

    ListOSFiles( pakfile, ".sdb", pakfiles ); // xAI: Prefer .sdb; .pk4 for migration
    ListOSFiles( pakfile, ".pk4", pakfiles );

    pakfiles.Sort();

    for ( i = 0; i < pakfiles.Num(); i++ ) {
        pakfile = BuildOSPath( path, dir, pakfiles[i] );
        uint32 checksum = MD4_BlockChecksumFile( pakfile ); // Existing checksum logic
        idStreamDbPack *sdb = new idStreamDbPack( pakfile.c_str(), checksum );
        if ( sdb ) {
            search = new searchpath_t;
            search->dir = NULL;
            search->pack = static_cast<pack_t*>(sdb);
            search->next = searchPaths->next;
            searchPaths->next = search;
            common->Printf( "Loaded sdb %s with checksum 0x%x\n", pakfile.c_str(), checksum );
        }
    }
}

/*
================
idFileSystemLocal::SetupGameDirectories
================
*/
void idFileSystemLocal::SetupGameDirectories( const char *gameName ) {
    if ( fs_cdpath.GetString()[0] ) {
        AddGameDirectory( fs_cdpath.GetString(), gameName );
    }

    if ( fs_basepath.GetString()[0] ) {
        AddGameDirectory( fs_basepath.GetString(), gameName );
    }

    if ( fs_devpath.GetString()[0] ) {
        AddGameDirectory( fs_devpath.GetString(), gameName );
    }

    if ( fs_savepath.GetString()[0] ) {
        AddGameDirectory( fs_savepath.GetString(), gameName );
    }

    if ( fs_configpath.GetString()[0] ) {
        AddGameDirectory( fs_configpath.GetString(), gameName );
    }
}

/*
================
idFileSystemLocal::FollowAddonDependencies
================
*/
void idFileSystemLocal::FollowAddonDependencies( pack_t *pak ) {
    int i, num;
    if ( !pak->addon_info ) {
        return;
    }
    num = pak->addon_info->depends.Num();
    for ( i = 0; i < num; i++ ) {
        pack_t *deppak = GetPackForChecksum( pak->addon_info->depends[i], true );
        if ( deppak ) {
            if ( !deppak->addon_search ) {
                int addon_index = addonChecksums.FindIndex( deppak->checksum );
                if ( addon_index >= 0 ) {
                    addonChecksums.RemoveIndex( addon_index );
                }
                deppak->addon_search = true;
                common->Printf( "Addon pk4 %s 0x%x depends on pak %s 0x%x, will be searched\n",
                                pak->pakFilename.c_str(), pak->checksum,
                                deppak->pakFilename.c_str(), deppak->checksum );
                FollowAddonDependencies( deppak );
            }
        } else {
            common->Printf( "Addon pk4 %s 0x%x depends on unknown pak 0x%x\n",
                            pak->pakFilename.c_str(), pak->checksum, pak->addon_info->depends[i] );
        }
    }
}

/*
================
idFileSystemLocal::GetPackStatus
================
*/
void idFileSystemLocal::GetPackStatus( pack_t *pak ) {
    if ( !pak->pureStatus ) {
        if ( restartChecksums.Num() ) {
            if ( restartChecksums.FindIndex( pak->checksum ) != -1 ) {
                pak->pureStatus = PURE_ALWAYS;
            } else {
                pak->pureStatus = PURE_NEVER;
            }
        } else {
            pak->pureStatus = PURE_NEVER;
        }
    }
}

/*
============
idFileSystemLocal::OSPathToRelativePath
================
*/
const char *idFileSystemLocal::OSPathToRelativePath( const char *OSPath ) {
    static char relativePath[MAX_OSPATH];
    const char *base = NULL;
    const char *s;

    // fs_game and fs_game_base support - look for first complete name with a mod path
    const char *fsgame = NULL;
    int igame = 0;
    for ( igame = 0; igame < 2; igame++ ) {
        if ( igame == 0 ) {
            fsgame = fs_game.GetString();
        } else if ( igame == 1 ) {
            fsgame = fs_game_base.GetString();
        }
        if ( base == NULL && fsgame && strlen( fsgame ) ) {
            base = strstr( OSPath, fsgame );
            while ( base ) {
                char c1 = '\0', c2;
                if ( base > OSPath ) {
                    c1 = *(base - 1);
                }
                c2 = *( base + strlen( fsgame ) );
                if ( ( c1 == '/' || c1 == '\\' ) && ( c2 == '/' || c2 == '\\' ) ) {
                    break;
                }
                base = strstr( base + 1, fsgame );
            }
        }
    }

    if ( base ) {
        s = strstr( base, ".pk4/" );
        if ( s != NULL ) {
            s += 4; // skip ".pk4"
        } else if ( idStr::Icmp( OSPath + idStr::Length(OSPath) - 4, ".sdb" ) == 0 ) {
            s = strchr( base, '/' );
            if ( s == NULL ) {
                s = strchr( base, '\\' );
            }
        } else {
            s = strchr( base, '/' );
            if ( s == NULL ) {
                s = strchr( base, '\\' );
            }
        }

        if ( s ) {
            strcpy( relativePath, s + 1 );
            if ( fs_debug.GetInteger() > 1 ) {
                common->Printf( "idFileSystem::OSPathToRelativePath: %s becomes %s\n", OSPath, relativePath );
            }
            return relativePath;
        }
    }

    common->Warning( "idFileSystem::OSPathToRelativePath failed on %s", OSPath );

    strcpy( relativePath, "" );
    return relativePath;
}

/*
=====================
idFileSystemLocal::RelativePathToOSPath
=====================
*/
const char *idFileSystemLocal::RelativePathToOSPath( const char *relativePath, const char *basePath ) {
    const char *path = cvarSystem->GetCVarString( basePath );
    if ( !path[0] ) {
        path = fs_savepath.GetString();
    }
    return BuildOSPath( path, gameFolder, relativePath );
}

/*
=================
idFileSystemLocal::RemoveFile
=================
*/
void idFileSystemLocal::RemoveFile( const char *relativePath ) {
    idStr OSPath;

    if ( fs_devpath.GetString()[0] ) {
        OSPath = BuildOSPath( fs_devpath.GetString(), gameFolder, relativePath );
        remove( OSPath );
    }

    OSPath = BuildOSPath( fs_savepath.GetString(), gameFolder, relativePath );
    remove( OSPath );

    ClearDirCache();
}

/*
================
idFileSystemLocal::FileIsInPAK
================
*/
bool idFileSystemLocal::FileIsInPAK( const char *relativePath ) {
    searchpath_t *search;
    pack_t *pak;
    int hash;

    if ( !searchPaths ) {
        common->FatalError( "Filesystem call made without initialization\n" );
    }

    if ( !relativePath ) {
        common->FatalError( "idFileSystemLocal::FileIsInPAK: NULL 'relativePath' parameter passed\n" );
    }

    if ( relativePath[0] == '/' || relativePath[0] == '\\' ) {
        relativePath++;
    }

    if ( strstr( relativePath, ".." ) || strstr( relativePath, "::" ) ) {
        return false;
    }

    hash = HashFileName( relativePath );

    for ( search = searchPaths; search; search = search->next ) {
        if ( search->pack && search->pack->hashTable[hash] ) {
            idStreamDbPack *sdb = static_cast<idStreamDbPack*>(search->pack);
            if ( sdb->Contains(relativePath) ) {
                return true;
            }
        }
    }
    return false;
}

/*
================
idFileSystemLocal::OpenFileReadFlags
================
*/
idFile *idFileSystemLocal::OpenFileReadFlags( const char *relativePath, int searchFlags, pack_t **foundInPak, bool *isConfig, pack_t **referencedPak, bool disableCopyFiles ) {
    searchpath_t *search;
    idStr netpath;
    pack_t *pak;
    idFile_Permanent *file;
    int hash;
    bool foundInDir = false;
    bool foundInPak = false;

    if ( !searchPaths ) {
        common->FatalError( "Filesystem call made without initialization\n" );
    }

    if ( !relativePath ) {
        common->FatalError( "idFileSystemLocal::OpenFileReadFlags: NULL 'relativePath' parameter passed\n" );
    }

    if ( relativePath[0] == '/' || relativePath[0] == '\\' ) {
        relativePath++;
    }

    if ( strstr( relativePath, ".." ) || strstr( relativePath, "::" ) ) {
        return NULL;
    }

    hash = HashFileName( relativePath );

    for ( search = searchPaths; search; search = search->next ) {
        if ( search->pack && ( searchFlags & FSFLAG_SEARCH_PAKS ) ) {
            idStreamDbPack *sdb = static_cast<idStreamDbPack*>(search->pack);
            if ( !sdb->Contains(relativePath) ) {
                continue;
            }

            if ( serverPaks.Num() ) {
                GetPackStatus( search->pack );
                if ( search->pack->pureStatus != PURE_NEVER && !serverPaks.Find( search->pack ) ) {
                    continue;
                }
            }

            file = static_cast<idFile_Permanent*>(sdb->GetFile(relativePath));
            if ( !file ) {
                continue;
            }

            foundInPak = true;
            if ( foundInPak ) {
                *foundInPak = search->pack;
            }

            if ( !search->pack->referenced && !( searchFlags & FSFLAG_PURE_NOREF ) ) {
                if ( fs_debug.GetInteger() ) {
                    common->Printf( "idFileSystem::OpenFileRead: %s -> adding %s to referenced paks\n", relativePath, search->pack->pakFilename.c_str() );
                }
                search->pack->referenced = true;
            }

            if ( fs_debug.GetInteger() ) {
                common->Printf( "idFileSystem::OpenFileRead: %s (found in sdb '%s')\n", relativePath, search->pack->pakFilename.c_str() );
            }
            return file;
        }

        if ( search->dir ) {
            if ( !foundInPak ) {
                netpath = BuildOSPath( search->dir->path, search->dir->gamedir, relativePath );
                file = new idFile_Permanent();
                file->o = OpenOSFile( netpath, "rb" );
                if ( file->o ) {
                    foundInDir = true;
                    file->fullPath = netpath.ToString();
                    file->name = relativePath;
                    file->mode = ( 1 << FS_READ );
                    file->fileSize = DirectFileLength( file->o );
                    file->handleSync = false;
                    if ( fs_debug.GetInteger() ) {
                        common->Printf( "idFileSystem::OpenFileRead: %s (found in dir '%s')\n", relativePath, netpath.c_str() );
                    }
                    return file;
                }
                delete file;
            }

            if ( !disableCopyFiles ) {
                const char *copypath = BuildOSPath( fs_savepath.GetString(), gameFolder, relativePath );
                const char *netpath = BuildOSPath( search->dir->path, search->dir->gamedir, relativePath );

                bool isFromCDPath = !search->dir->path.Cmp( fs_cdpath.GetString() );
                bool isFromSavePath = !search->dir->path.Cmp( fs_savepath.GetString() );
                bool isFromBasePath = !search->dir->path.Cmp( fs_basepath.GetString() );

                switch ( fs_copyfiles.GetInteger() ) {
                    case 1:
                        if ( isFromCDPath ) {
                            CopyFile( netpath, copypath );
                        }
                        break;
                    case 2:
                        if ( isFromCDPath ) {
                            CopyFile( netpath, copypath );
                        } else if ( isFromSavePath || isFromBasePath ) {
                            idStr sourcepath;
                            sourcepath = BuildOSPath( fs_cdpath.GetString(), search->dir->gamedir, relativePath );
                            FILE *f1 = OpenOSFile( sourcepath, "r" );
                            if ( f1 ) {
                                ID_TIME_T t1 = Sys_FileTimeStamp( f1 );
                                fclose( f1 );
                                FILE *f2 = OpenOSFile( copypath, "r" );
                                if ( f2 ) {
                                    ID_TIME_T t2 = Sys_FileTimeStamp( f2 );
                                    fclose( f2 );
                                    if ( t1 > t2 ) {
                                        CopyFile( sourcepath, copypath );
                                    }
                                }
                            }
                        }
                        break;
                    case 3:
                        if ( isFromCDPath || isFromBasePath ) {
                            CopyFile( netpath, copypath );
                        }
                        break;
                    case 4:
                        if ( isFromCDPath && !isFromBasePath ) {
                            CopyFile( netpath, copypath );
                        }
                        break;
                }
            }
        }
    }

    if ( searchFlags & FSFLAG_SEARCH_ADDONS ) {
        for ( search = addonPaks; search; search = search->next ) {
            idStreamDbPack *sdb = static_cast<idStreamDbPack*>(search->pack);
            if ( !sdb->Contains(relativePath) ) {
                continue;
            }
            idFile *file = sdb->GetFile(relativePath);
            if ( file ) {
                if ( foundInPak ) {
                    *foundInPak = search->pack;
                }
                if ( fs_debug.GetInteger() ) {
                    common->Printf( "idFileSystem::OpenFileRead: %s (found in addon sdb '%s')\n", relativePath, search->pack->pakFilename.c_str() );
                }
                return file;
            }
        }
    }

    if ( fs_debug.GetInteger() ) {
        common->Printf( "Can't find %s\n", relativePath );
    }

    return NULL;
}

/*
===========
idFileSystemLocal::OpenFileRead
===========
*/
idFile *idFileSystemLocal::OpenFileRead( const char *relativePath, bool allowCopyFiles ) {
    return OpenFileReadFlags( relativePath, FSFLAG_SEARCH_DIRS | FSFLAG_SEARCH_PAKS | FSFLAG_SEARCH_ADDONS, NULL, NULL, NULL, !allowCopyFiles );
}

/*
============
idFileSystemLocal::ReadFile
============
*/
int idFileSystemLocal::ReadFile( const char *relativePath, void **buffer, ID_TIME_T *timestamp ) {
    idFile *f;
    byte *buf;
    int len;
    bool isConfig;

    if ( !searchPaths ) {
        common->FatalError( "Filesystem call made without initialization\n" );
    }

    if ( !relativePath || !relativePath[0] ) {
        common->FatalError( "idFileSystemLocal::ReadFile with empty name\n" );
    }

    if ( timestamp ) {
        *timestamp = FILE_NOT_FOUND_TIMESTAMP;
    }

    if ( buffer ) {
        *buffer = NULL;
    }

    buf = NULL;

    if ( strstr( relativePath, ".cfg" ) == relativePath + strlen( relativePath ) - 4 ) {
        isConfig = true;
        if ( eventLoop && eventLoop->JournalLevel() == 2 ) {
            int r;

            loadCount++;
            loadStack++;

            common->DPrintf( "Loading %s from journal file.\n", relativePath );
            len = 0;
            r = eventLoop->com_journalDataFile->Read( &len, sizeof( len ) );
            if ( r != sizeof( len ) ) {
                *buffer = NULL;
                return -1;
            }
            buf = (byte *)Mem_ClearedAlloc(len+1);
            *buffer = buf;
            r = eventLoop->com_journalDataFile->Read( buf, len );
            if ( r != len ) {
                common->FatalError( "Read from journalDataFile failed" );
            }

            buf[len] = 0;

            return len;
        }
    } else {
        isConfig = false;
    }

    f = OpenFileRead( relativePath, ( buffer != NULL ) );
    if ( f == NULL ) {
        if ( buffer ) {
            *buffer = NULL;
        }
        return -1;
    }
    len = f->Length();

    if ( timestamp ) {
        *timestamp = f->Timestamp();
    }

    if ( !buffer ) {
        CloseFile( f );
        return len;
    }

    loadCount++;
    loadStack++;

    buf = (byte *)Mem_ClearedAlloc(len+1);
    *buffer = buf;

    f->Read( buf, len );

    buf[len] = 0;
    CloseFile( f );

    if ( isConfig && eventLoop && eventLoop->JournalLevel() == 1 ) {
        common->DPrintf( "Writing %s to journal file.\n", relativePath );
        eventLoop->com_journalDataFile->Write( &len, sizeof( len ) );
        eventLoop->com_journalDataFile->Write( buf, len );
        eventLoop->com_journalDataFile->Flush();
    }

    return len;
}

/*
=============
idFileSystemLocal::FreeFile
=============
*/
void idFileSystemLocal::FreeFile( void *buffer ) {
    if ( !searchPaths ) {
        common->FatalError( "Filesystem call made without initialization\n" );
    }
    if ( !buffer ) {
        common->FatalError( "idFileSystemLocal::FreeFile( NULL )" );
    }
    loadStack--;

    Mem_Free( buffer );
}

/*
============
idFileSystemLocal::WriteFile
============
*/
int idFileSystemLocal::WriteFile( const char *relativePath, const void *buffer, int size, const char *basePath ) {
    idFile *f;

    if ( !searchPaths ) {
        common->FatalError( "Filesystem call made without initialization\n" );
    }

    if ( !relativePath || !buffer ) {
        common->FatalError( "idFileSystemLocal::WriteFile: NULL parameter" );
    }

    f = OpenFileWrite( relativePath, basePath );
    if ( !f ) {
        return -1;
    }

    f->Write( buffer, size );
    CloseFile( f );

    return size;
}

/*
================
idFileSystemLocal::OpenFileWrite
================
*/
idFile *idFileSystemLocal::OpenFileWrite( const char *relativePath, const char *basePath ) {
    const char *path;
    idStr OSpath;
    idFile_Permanent *f;

    if ( !searchPaths ) {
        common->FatalError( "Filesystem call made without initialization\n" );
    }

    path = cvarSystem->GetCVarString( basePath );
    if ( !path[0] ) {
        path = fs_savepath.GetString();
    }

    OSpath = BuildOSPath( path, gameFolder, relativePath );

    if ( fs_debug.GetInteger() ) {
        common->Printf( "idFileSystem::OpenFileWrite: %s\n", OSpath.c_str() );
    }

    ClearDirCache();

    common->DPrintf( "writing to: %s\n", OSpath.c_str() );
    CreateOSPath( OSpath );

    f = new idFile_Permanent();
    f->o = OpenOSFile( OSpath, "wb" );
    if ( !f->o ) {
        delete f;
        return NULL;
    }
    f->name = relativePath;
    f->fullPath = OSpath;
    f->mode = ( 1 << FS_WRITE );
    f->handleSync = false;
    f->fileSize = 0;

    return f;
}

/*
===========
idFileSystemLocal::OpenExplicitFileRead
===========
*/
idFile *idFileSystemLocal::OpenExplicitFileRead( const char *OSPath ) {
    idFile_Permanent *f;

    if ( !searchPaths ) {
        common->FatalError( "Filesystem call made without initialization\n" );
    }

    if ( fs_debug.GetInteger() ) {
        common->Printf( "idFileSystem::OpenExplicitFileRead: %s\n", OSPath );
    }

    common->DPrintf( "idFileSystem::OpenExplicitFileRead - reading from: %s\n", OSPath );

    f = new idFile_Permanent();
    f->o = OpenOSFile( OSPath, "rb" );
    if ( !f->o ) {
        delete f;
        return NULL;
    }
    f->name = OSPath;
    f->fullPath = OSPath;
    f->mode = ( 1 << FS_READ );
    f->handleSync = false;
    f->fileSize = DirectFileLength( f->o );

    return f;
}

/*
===========
idFileSystemLocal::OpenExplicitFileWrite
===========
*/
idFile *idFileSystemLocal::OpenExplicitFileWrite( const char *OSPath ) {
    idFile_Permanent *f;

    if ( !searchPaths ) {
        common->FatalError( "Filesystem call made without initialization\n" );
    }

    if ( fs_debug.GetInteger() ) {
        common->Printf( "idFileSystem::OpenExplicitFileWrite: %s\n", OSPath );
    }

    common->DPrintf( "writing to: %s\n", OSPath );
    CreateOSPath( OSPath );

    f = new idFile_Permanent();
    f->o = OpenOSFile( OSPath, "wb" );
    if ( !f->o ) {
        delete f;
        return NULL;
    }
    f->name = OSPath;
    f->fullPath = OSPath;
    f->mode = ( 1 << FS_WRITE );
    f->handleSync = false;
    f->fileSize = 0;

    return f;
}

/*
===========
idFileSystemLocal::OpenFileAppend
===========
*/
idFile *idFileSystemLocal::OpenFileAppend( const char *relativePath, bool sync, const char *basePath ) {
    const char *path;
    idStr OSpath;
    idFile_Permanent *f;

    if ( !searchPaths ) {
        common->FatalError( "Filesystem call made without initialization\n" );
    }

    path = cvarSystem->GetCVarString( basePath );
    if ( !path[0] ) {
        path = fs_savepath.GetString();
    }

    OSpath = BuildOSPath( path, gameFolder, relativePath );
    CreateOSPath( OSpath );

    if ( fs_debug.GetInteger() ) {
        common->Printf( "idFileSystem::OpenFileAppend: %s\n", OSpath.c_str() );
    }

    f = new idFile_Permanent();
    f->o = OpenOSFile( OSpath, "ab" );
    if ( !f->o ) {
        delete f;
        return NULL;
    }
    f->name = relativePath;
    f->fullPath = OSpath;
    f->mode = ( 1 << FS_WRITE ) + ( 1 << FS_APPEND );
    f->handleSync = sync;
    f->fileSize = DirectFileLength( f->o );

    return f;
}

/*
================
idFileSystemLocal::OpenFileByMode
================
*/
idFile *idFileSystemLocal::OpenFileByMode( const char *relativePath, fsMode_t mode ) {
    if ( mode == FS_READ ) {
        return OpenFileRead( relativePath );
    }
    if ( mode == FS_WRITE ) {
        return OpenFileWrite( relativePath );
    }
    if ( mode == FS_APPEND ) {
        return OpenFileAppend( relativePath, true );
    }
    common->FatalError( "idFileSystemLocal::OpenFileByMode: bad mode" );
    return NULL;
}

/*
==============
idFileSystemLocal::CloseFile
==============
*/
void idFileSystemLocal::CloseFile( idFile *f ) {
    if ( !searchPaths ) {
        common->FatalError( "Filesystem call made without initialization\n" );
    }
    if ( !f ) {
        return;
    }
    delete f;
}

/*
===============
idFileSystemLocal::FindFile
===============
*/
findFile_t idFileSystemLocal::FindFile( const char *path, bool scheduleAddons ) {
    pack_t *pak;
    idFile *f = OpenFileReadFlags( path, FSFLAG_SEARCH_DIRS | FSFLAG_SEARCH_PAKS | FSFLAG_SEARCH_ADDONS, &pak );
    if ( !f ) {
        return FIND_NO;
    }
    if ( !pak ) {
        return FIND_YES;
    }
    if ( scheduleAddons && pak->addon && addonChecksums.FindIndex( pak->checksum ) < 0 ) {
        addonChecksums.Append( pak->checksum );
    }
    if ( pak->addon && !pak->addon_search ) {
        delete f;
        return FIND_ADDON;
    }
    delete f;
    return FIND_YES;
}

/*
===============
idFileSystemLocal::GetNumMaps
===============
*/
int idFileSystemLocal::GetNumMaps() {
    int i;
    searchpath_t *search = NULL;
    int ret = declManager->GetNumDecls( DECL_MAPDEF );

    for ( i = 0; i < 2; i++ ) {
        if ( i == 0 ) {
            search = searchPaths;
        } else if ( i == 1 ) {
            search = addonPaks;
        }
        for ( ; search ; search = search->next ) {
            if ( !search->pack || !search->pack->addon || !search->pack->addon_info ) {
                continue;
            }
            ret += search->pack->addon_info->mapDecls.Num();
        }
    }
    return ret;
}

/*
===============
idFileSystemLocal::GetMapDecl
===============
*/
const idDict * idFileSystemLocal::GetMapDecl( int idecl ) {
    int i;
    const idDecl *mapDecl;
    const idDeclEntityDef *mapDef;
    int numdecls = declManager->GetNumDecls( DECL_MAPDEF );
    searchpath_t *search = NULL;

    if ( idecl < numdecls ) {
        mapDecl = declManager->DeclByIndex( DECL_MAPDEF, idecl );
        mapDef = static_cast<const idDeclEntityDef *>( mapDecl );
        if ( !mapDef ) {
            common->Error( "idFileSystemLocal::GetMapDecl %d: not found\n", idecl );
        }
        mapDict = mapDef->dict;
        mapDict.Set( "path", mapDef->GetName() );
        return &mapDict;
    }
    idecl -= numdecls;
    for ( i = 0; i < 2; i++ ) {
        if ( i == 0 ) {
            search = searchPaths;
        } else if ( i == 1 ) {
            search = addonPaks;
        }
        for ( ; search ; search = search->next ) {
            if ( !search->pack || !search->pack->addon || !search->pack->addon_info ) {
                continue;
            }
            if ( idecl < search->pack->addon_info->mapDecls.Num() ) {
                mapDict = *search->pack->addon_info->mapDecls[ idecl ];
                return &mapDict;
            }
            idecl -= search->pack->addon_info->mapDecls.Num();
            assert( idecl >= 0 );
        }
    }
    return NULL;
}

/*
===============
idFileSystemLocal::FindMapScreenshot
===============
*/
void idFileSystemLocal::FindMapScreenshot( const char *path, char *buf, int len ) {
    idFile *file;
    idStr mapname = path;

    mapname.StripPath();
    mapname.StripFileExtension();

    idStr::snPrintf( buf, len, "guis/assets/splash/%s.tga", mapname.c_str() );
    if ( ReadFile( buf, NULL, NULL ) == -1 ) {
        file = OpenFileReadFlags( buf, FSFLAG_SEARCH_ADDONS );
        if ( file ) {
            int dlen = file->Length();
            char *data = new char[ dlen ];
            file->Read( data, dlen );
            CloseFile( file );
            idStr::snPrintf( buf, len, "guis/assets/splash/addon/%s.tga", mapname.c_str() );
            WriteFile( buf, data, dlen );
            delete[] data;
        } else {
            idStr::Copynz( buf, "guis/assets/splash/pdtempa", len );
        }
    }
}

/*
============
idFileSystemLocal::TouchFile_f
============
*/
void idFileSystemLocal::TouchFile_f( const idCmdArgs &args ) {
    idFile *f;

    if ( args.Argc() != 2 ) {
        common->Printf( "Usage: touchFile <file>\n" );
        return;
    }

    f = fileSystemLocal.OpenFileRead( args.Argv( 1 ) );
    if ( f ) {
        fileSystemLocal.CloseFile( f );
    }
}

/*
============
idFileSystemLocal::TouchFileList_f
============
*/
void idFileSystemLocal::TouchFileList_f( const idCmdArgs &args ) {
    if ( args.Argc() != 2 ) {
        common->Printf( "Usage: touchFileList <filename>\n" );
        return;
    }

    const char *buffer = NULL;
    idParser src( LEXFL_NOFATALERRORS | LEXFL_NOSTRINGCONCAT | LEXFL_ALLOWMULTICHARLITERALS | LEXFL_ALLOWBACKSLASHSTRINGCONCAT );
    if ( fileSystem->ReadFile( args.Argv( 1 ), ( void** )&buffer, NULL ) && buffer ) {
        src.LoadMemory( buffer, strlen( buffer ), args.Argv( 1 ) );
        if ( src.IsLoaded() ) {
            idToken token;
            while( src.ReadToken( &token ) ) {
                common->Printf( "%s\n", token.c_str() );
                session->UpdateScreen();
                idFile *f = fileSystemLocal.OpenFileRead( token );
                if ( f ) {
                    fileSystemLocal.CloseFile( f );
                }
            }
        }
    }
}

/*
============
idFileSystemLocal::BuildSdb_f
============
*/
void idFileSystemLocal::BuildSdb_f( const idCmdArgs &args ) {
    if ( args.Argc() < 3 ) {
        common->Printf( "Usage: buildSdb <input_dir> <output.sdb>\n" );
        return;
    }
    idStr inputDir = args.Argv(1);
    idStr outputSdb = args.Argv(2);
    idStreamDbPack *sdb = new idStreamDbPack(outputSdb.c_str(), 0);
    idStrList files;
    ListFilesTree(inputDir, "*.*", files);
    for (idStr file : files) {
        idFile *f = OpenFileRead(file.c_str());
        if (f) {
            byte *data = new byte[f->Length()];
            f->Read(data, f->Length());
            rust::Vec<u8> rustData(data, f->Length());
            auto result = sdb->db->write_document(&rust::String(file.c_str()), &rustData);
            if (result.is_err()) {
                common->Warning("Failed to write %s to sdb", file.c_str());
            }
            delete[] data;
            CloseFile(f);
        }
    }
    common->Printf("Built %s with %d files\n", outputSdb.c_str(), files.Num());
}

/*
================
idFileSystemLocal::ListFiles
================
*/
idFileList *idFileSystemLocal::ListFiles( const char *relativePath, const char *extension, bool sort ) {
    idStrList files;
    searchpath_t *search;

    if ( !searchPaths ) {
        common->FatalError( "Filesystem call made without initialization\n" );
    }

    if ( !relativePath ) {
        return NULL;
    }

    for ( search = searchPaths; search; search = search->next ) {
        if ( search->dir ) {
            idStr path = BuildOSPath( search->dir->path, search->dir->gamedir, relativePath );
            idStrList dirFiles;
            ListOSFiles( path, extension, dirFiles );
            for ( int i = 0; i < dirFiles.Num(); i++ ) {
                files.AddUnique( dirFiles[i] );
            }
        } else if ( search->pack ) {
            idStreamDbPack *sdb = static_cast<idStreamDbPack*>(search->pack);
            idStrList sdbFiles = sdb->ListFiles(relativePath, extension);
            for ( int i = 0; i < sdbFiles.Num(); i++ ) {
                files.AddUnique( sdbFiles[i] );
            }
        }
    }

    if ( sort ) {
        files.Sort();
    }

    idFileList *fileList = new idFileList;
    fileList->list = files;
    return fileList;
}

/*
================
idFileSystemLocal::ListFilesTree
================
*/
idFileList *idFileSystemLocal::ListFilesTree( const char *relativePath, const char *extension, bool sort ) {
    idStrList files;
    searchpath_t *search;

    if ( !searchPaths ) {
        common->FatalError( "Filesystem call made without initialization\n" );
    }

    if ( !relativePath ) {
        return NULL;
    }

    for ( search = searchPaths; search; search = search->next ) {
        if ( search->dir ) {
            idStr path = BuildOSPath( search->dir->path, search->dir->gamedir, relativePath );
            idStrList dirFiles;
            ListOSFilesTree( path, extension, dirFiles );
            for ( int i = 0; i < dirFiles.Num(); i++ ) {
                files.AddUnique( dirFiles[i] );
            }
        } else if ( search->pack ) {
            idStreamDbPack *sdb = static_cast<idStreamDbPack*>(search->pack);
            idStrList sdbFiles = sdb->ListFiles(relativePath, extension);
            for ( int i = 0; i < sdbFiles.Num(); i++ ) {
                files.AddUnique( sdbFiles[i] );
            }
        }
    }

    if ( sort ) {
        files.Sort();
    }

    idFileList *fileList = new idFileList;
    fileList->list = files;
    return fileList;
}

/*
================
idFileSystemLocal::FreeFileList
================
*/
void idFileSystemLocal::FreeFileList( idFileList *f ) {
    if ( f ) {
        delete f;
    }
}

/*
================
idFileSystemLocal::Dir_f
================
*/
void idFileSystemLocal::Dir_f( const idCmdArgs &args ) {
    idStr relativePath, extension;
    idFileList *fileList;

    if ( args.Argc() < 2 || args.Argc() > 3 ) {
        common->Printf( "usage: dir <directory> [extension]\n" );
        return;
    }

    if ( args.Argc() == 2 ) {
        relativePath = args.Argv( 1 );
        extension = "";
    } else {
        relativePath = args.Argv( 1 );
        extension = args.Argv( 2 );
        if ( extension[0] != '.' ) {
            common->Warning( "extension should have a leading dot" );
        }
    }
    relativePath.BackSlashesToSlashes();
    relativePath.StripTrailing( '/' );

    common->Printf( "Listing of %s/*%s\n", relativePath.c_str(), extension.c_str() );
    common->Printf( "---------------\n" );

    fileList = fileSystemLocal.ListFiles( relativePath, extension );

    for ( int i = 0; i < fileList->GetNumFiles(); i++ ) {
        common->Printf( "%s\n", fileList->GetFile( i ) );
    }
    common->Printf( "%d files\n", fileList->list.Num() );

    fileSystemLocal.FreeFileList( fileList );
}

/*
================
idFileSystemLocal::DirTree_f
================
*/
void idFileSystemLocal::DirTree_f( const idCmdArgs &args ) {
    idStr relativePath, extension;
    idFileList *fileList;

    if ( args.Argc() < 2 || args.Argc() > 3 ) {
        common->Printf( "usage: dirtree <directory> [extension]\n" );
        return;
    }

    if ( args.Argc() == 2 ) {
        relativePath = args.Argv( 1 );
        extension = "";
    } else {
        relativePath = args.Argv( 1 );
        extension = args.Argv( 2 );
        if ( extension[0] != '.' ) {
            common->Warning( "extension should have a leading dot" );
        }
    }
    relativePath.BackSlashesToSlashes();
    relativePath.StripTrailing( '/' );

    common->Printf( "Listing of %s/*%s /s\n", relativePath.c_str(), extension.c_str() );
    common->Printf( "---------------\n" );

    fileList = fileSystemLocal.ListFilesTree( relativePath, extension );

    for ( int i = 0; i < fileList->GetNumFiles(); i++ ) {
        common->Printf( "%s\n", fileList->GetFile( i ) );
    }
    common->Printf( "%d files\n", fileList->list.Num() );

    fileSystemLocal.FreeFileList( fileList );
}

/*
============
idFileSystemLocal::Path_f
============
*/
void idFileSystemLocal::Path_f( const idCmdArgs &args ) {
    common->Printf( "Current search path:\n" );
    for ( searchpath_t *search = searchPaths; search; search = search->next ) {
        common->Printf( "%s/%s\n", search->dir ? search->dir->path.c_str() : "", search->dir ? search->dir->gamedir.c_str() : search->pack->pakFilename.c_str() );
    }
}

/*
=================
idFileSystemLocal::FindDLL
=================
*/
void idFileSystemLocal::FindDLL( const char *name, char *dllPath ) {
    char dllName[MAX_OSPATH];
    sys->DLL_GetFileName( name, dllName, MAX_OSPATH );

    idStr dllPathStr = Sys_EXEPath();
    dllPathStr.StripFilename();
    dllPathStr.AppendPath( dllName );
    idFile *dllFile = OpenExplicitFileRead( dllPathStr );

    if ( dllFile ) {
        dllPathStr = dllFile->GetFullPath();
        CloseFile( dllFile );
        dllFile = NULL;
    } else {
        dllPathStr = "";
    }
    idStr::snPrintf( dllPath, MAX_OSPATH, dllPathStr.c_str() );
}

/*
================
idFileSystemLocal::ClearDirCache
================
*/
void idFileSystemLocal::ClearDirCache( void ) {
    // Implement dir cache clearing if needed
}

/*
================
idFileSystemLocal::BuildOSPath
================
*/
const char *idFileSystemLocal::BuildOSPath( const char *base, const char *game, const char *relative ) {
    static char OSPath[MAX_OSPATH];
    idStr newPath;

    if ( !game || !game[0] ) {
        newPath = va( "%s/%s", base, relative );
    } else {
        newPath = va( "%s/%s/%s", base, game, relative );
    }

    idStr::Copynz( OSPath, newPath, MAX_OSPATH );
    idStr::ReplaceChar( OSPath, '\\', '/' );
    return OSPath;
}

/*
================
idFileSystemLocal::ListOSFiles
================
*/
void idFileSystemLocal::ListOSFiles( const char *directory, const char *extension, idStrList &list ) {
    // Implement OS file listing (platform-specific)
}

/*
================
idFileSystemLocal::ListOSFilesTree
================
*/
void idFileSystemLocal::ListOSFilesTree( const char *directory, const char *extension, idStrList &list ) {
    // Implement recursive OS file listing
}

/*
================
idFileSystemLocal::HashFileName
================
*/
int idFileSystemLocal::HashFileName( const char *fname ) {
    // Implement hash function (existing logic)
    return 0; // Placeholder
}

/*
================
idFileSystemLocal::CopyFile
================
*/
void idFileSystemLocal::CopyFile( const char *fromOSPath, const char *toOSPath ) {
    idFile *src = OpenExplicitFileRead( fromOSPath );
    if ( !src ) {
        common->Warning( "could not open source file %s\n", fromOSPath );
        return;
    }

    idFile *dst = OpenExplicitFileWrite( toOSPath );
    if ( !dst ) {
        common->Warning( "could not open destination file %s\n", toOSPath );
        CloseFile( src );
        return;
    }

    int len = src->Length();
    byte *buffer = new byte[len];
    src->Read( buffer, len );
    dst->Write( buffer, len );

    delete[] buffer;
    CloseFile( src );
    CloseFile( dst );
}

/*
================
idFileSystemLocal::OpenOSFile
================
*/
FILE *idFileSystemLocal::OpenOSFile( const char *fileName, const char *mode ) {
    return fopen( fileName, mode );
}

/*
================
idFileSystemLocal::DirectFileLength
================
*/
int idFileSystemLocal::DirectFileLength( FILE *o ) {
    int pos, end;

    pos = ftell( o );
    fseek( o, 0, SEEK_END );
    end = ftell( o );
    fseek( o, pos, SEEK_SET );
    return end;
}

/*
================
idFileSystemLocal::MD4_BlockChecksumFile
================
*/
uint32 idFileSystemLocal::MD4_BlockChecksumFile( const char *fileName ) {
    // Implement MD4 checksum (existing logic)
    return 0; // Placeholder
}

/*
================
idFileSystemLocal::GetPackForChecksum
================
*/
pack_t *idFileSystemLocal::GetPackForChecksum( uint32 checksum, bool searchAddons ) {
    searchpath_t *search;

    for ( search = searchPaths; search; search = search->next ) {
        if ( search->pack && search->pack->checksum == checksum ) {
            return search->pack;
        }
    }

    if ( searchAddons ) {
        for ( search = addonPaks; search; search = search->next ) {
            if ( search->pack && search->pack->checksum == checksum ) {
                return search->pack;
            }
        }
    }

    return NULL;
}
