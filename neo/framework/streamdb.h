// streamdb.h - FFI wrapper for StreamDB
#ifndef __STREAMDB_H__
#define __STREAMDB_H__

extern "C" {
    void* streamdb_open(const char* path);
    int streamdb_get(void* db, const char* path, unsigned char** out_data, unsigned int* out_len);
    int streamdb_search(void* db, const char* prefix, char*** out_paths, unsigned int* out_count);
    void streamdb_free_data(unsigned char* ptr);
    void streamdb_free_paths(char** ptr, unsigned int count);
    // Add declarations for write, delete, etc.
}

class idStreamDbHandle {
public:
    idStreamDbHandle(const idStr& path) : handle(streamdb_open(path.c_str())) {}
    ~idStreamDbHandle() { /* Add close if needed */ }
    idFile* GetFile(const idStr& relPath) {
        unsigned char* data;
        unsigned int len;
        if (streamdb_get(handle, relPath.c_str(), &data, &len) == 0) {
            idFile_Memory* file = new idFile_Memory(relPath, reinterpret_cast<const char*>(data), len);
            streamdb_free_data(data);
            return file;
        }
        return nullptr;
    }
    idStrList Search(const idStr& prefix) {
        char** paths;
        unsigned int count;
        if (streamdb_search(handle, prefix.c_str(), &paths, &count) == 0) {
            idStrList list;
            for (unsigned int i = 0; i < count; ++i) {
                list.Append(paths[i]);
            }
            streamdb_free_paths(paths, count);
            return list;
        }
        return idStrList();
    }
private:
    void* handle;
};

#endif
