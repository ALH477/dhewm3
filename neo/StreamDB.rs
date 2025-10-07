use cxx::{CxxString, CxxVector, UniquePtr, Pin};
use std::collections::{BTreeMap, VecDeque};
use std::fs::{File, OpenOptions};
use std::io::{self, BufReader, BufWriter, Cursor, Read, Seek, SeekFrom, Write};
use std::path::Path;
use std::sync::{Arc, Mutex, RwLock};
use parking_lot::{Mutex as PMutex, RwLock as PRwLock};
use memmap2::{MmapMut, MmapOptions};
use byteorder::{LittleEndian, ReadBytesExt, WriteBytesExt};
use uuid::Uuid;
use crc::Crc;
use crc::CRC_32_ISO_HDLC;
use lru::LruCache;
use snappy;
use md4::{Md4, Digest}; // Added for idTech4 checksum

const MAGIC: [u8; 8] = [0x55, 0xAA, 0xFE, 0xED, 0xFA, 0xCE, 0xDA, 0x7A];
const PAGE_SIZE: u64 = 4096; // idTech4-aligned (HDD)
const PAGE_HEADER_SIZE: u64 = 32; // crc(4) + version(4) + prev/next(8+8) + flags(1) + len(4) + pad(3)
const FREE_LIST_HEADER_SIZE: u64 = 12; // next(8) + used(4)
const FREE_LIST_ENTRIES_PER_PAGE: usize = ((PAGE_SIZE - PAGE_HEADER_SIZE - FREE_LIST_HEADER_SIZE) / 8) as usize; // ~1010
const MAX_PAGES: i64 = i64::MAX;
const MAX_DOCUMENT_SIZE: u64 = 256 * 1024 * 1024;
const BATCH_GROW_PAGES: u64 = 16;
const PAGE_CACHE_SIZE: usize = 2048;
const PATH_CACHE_SIZE: usize = 1024;
const VERSIONS_TO_KEEP: i32 = 2;
const MAX_CONSECUTIVE_EMPTY_FREE_LIST: i64 = 5;

#[derive(Clone, Debug)]
pub struct CacheStats {
    hits: usize,
    misses: usize,
}

#[derive(Clone)]
struct Config {
    page_size: u64,
    page_header_size: u64,
    max_pages: i64,
    max_document_size: u64,
    use_compression: bool,
    page_cache_size: usize,
    path_cache_size: usize,
    versions_to_keep: i32,
}

impl Default for Config {
    fn default() -> Self {
        Config {
            page_size: PAGE_SIZE,
            page_header_size: PAGE_HEADER_SIZE,
            max_pages: MAX_PAGES,
            max_document_size: MAX_DOCUMENT_SIZE,
            use_compression: true,
            page_cache_size: PAGE_CACHE_SIZE,
            path_cache_size: PATH_CACHE_SIZE,
            versions_to_keep: VERSIONS_TO_KEEP,
        }
    }
}

#[derive(Clone, Copy)]
struct PageHeader {
    crc: u32,
    version: i32,
    prev_page_id: i64,
    next_page_id: i64,
    flags: u8,
    data_length: i32,
    padding: [u8; 3],
}

const FLAG_DATA_PAGE: u8 = 0x01;
const FLAG_TRIE_PAGE: u8 = 0x02;
const FLAG_FREE_LIST_PAGE: u8 = 0x04;
const FLAG_INDEX_PAGE: u8 = 0x08;

#[derive(Clone)]
struct Document {
    id: Uuid,
    first_page_id: i64,
    current_version: i32,
    paths: Vec<String>,
}

#[derive(Clone)]
struct ReverseTrieNode {
    edge: String,
    parent_page_id: i64,
    self_page_id: i64,
    document_id: Option<Uuid>,
    children: BTreeMap<char, i64>, // Optimized: BTreeMap for persistence
}

struct VersionedLink {
    page_id: i64,
    version: i32,
}

struct Transaction {
    writes: VecDeque<(i64, Vec<u8>, i32)>, // page_id, data, version
    frees: Vec<i64>,
}

#[cxx::bridge]
mod ffi {
    #[derive(Clone, Debug)]
    struct CacheStats {
        hits: usize,
        misses: usize,
    }

    unsafe extern "C++" {
        include!("framework/Common.h");
        type idCommon;
        fn commonPrintf(self: &idCommon, fmt: &CxxString);
    }

    extern "Rust" {
        type StreamDb;

        fn open_db(path: &CxxString, use_compression: bool, quick_mode: bool) -> Result<UniquePtr<StreamDb>>;
        fn close_db(self: Pin<&mut StreamDb>);
        fn write_document(self: Pin<&mut StreamDb>, path: &CxxString, data: &CxxVector<u8>) -> Result<Uuid>;
        fn get(self: &StreamDb, path: &CxxString) -> Result<CxxVector<u8>>;
        fn search_paths(self: &StreamDb, prefix: &CxxString) -> Result<CxxVector<CxxString>>;
        fn delete_by_path(self: Pin<&mut StreamDb>, path: &CxxString) -> Result<()>;
        fn get_checksum(self: &StreamDb) -> u32;
        fn set_quick_mode(self: Pin<&mut StreamDb>, enabled: bool);
        fn get_cache_stats(self: &StreamDb) -> CacheStats;
        fn start_stream(self: &StreamDb, path: &CxxString) -> Result<i64>;
        fn next_stream_chunk(self: &StreamDb, stream_id: i64) -> Result<CxxVector<u8>>;
        fn end_stream(self: Pin<&mut StreamDb>, stream_id: i64);
        fn bind_addon_path(self: Pin<&mut StreamDb>, path: &CxxString, addon: bool) -> Result<()>;
        fn begin_transaction(self: Pin<&mut StreamDb>) -> Result<i64>;
        fn commit_transaction(self: Pin<&mut StreamDb>, tx_id: i64) -> Result<()>;
        fn rollback_transaction(self: Pin<&mut StreamDb>, tx_id: i64) -> Result<()>;
    }
}

pub struct StreamDb {
    config: Config,
    file: PMutex<File>,
    mmap: PRwLock<Option<MmapMut>>,
    current_size: PMutex<u64>,
    document_index_root: PRwLock<VersionedLink>,
    trie_root: PRwLock<VersionedLink>,
    free_list_root: PRwLock<VersionedLink>,
    page_cache: PMutex<LruCache<i64, Vec<u8>>>,
    path_cache: PMutex<LruCache<String, Uuid>>,
    cache_stats: PMutex<CacheStats>,
    quick_mode: Arc<std::sync::atomic::AtomicBool>,
    transactions: PMutex<Vec<Transaction>>,
}

impl StreamDb {
    pub fn open_db(path: &CxxString, use_compression: bool, quick_mode: bool) -> Result<UniquePtr<StreamDb>, std::io::Error> {
        let config = Config { use_compression, ..Default::default() };
        let file = OpenOptions::new().read(true).write(true).create(true).open(path.to_string_lossy())?;
        let mmap = if config.page_size >= 4096 {
            Some(unsafe { MmapOptions::new().len(config.page_size as usize * config.max_pages as usize).map_mut(&file)? })
        } else {
            None
        };
        let mut db = StreamDb {
            config,
            file: PMutex::new(file),
            mmap: PRwLock::new(mmap),
            current_size: PMutex::new(0),
            document_index_root: PRwLock::new(VersionedLink { page_id: -1, version: 0 }),
            trie_root: PRwLock::new(VersionedLink { page_id: -1, version: 0 }),
            free_list_root: PRwLock::new(VersionedLink { page_id: -1, version: 0 }),
            page_cache: PMutex::new(LruCache::new(config.page_cache_size)),
            path_cache: PMutex::new(LruCache::new(config.path_cache_size)),
            cache_stats: PMutex::new(CacheStats { hits: 0, misses: 0 }),
            quick_mode: Arc::new(std::sync::atomic::AtomicBool::new(quick_mode)),
            transactions: PMutex::new(Vec::new()),
        };
        db.initialize()?;
        Ok(cxx::UniquePtr::new(db))
    }

    fn initialize(&mut self) -> io::Result<()> {
        let mut file = self.file.lock();
        file.seek(SeekFrom::Start(0))?;
        let mut header = vec![0u8; 32]; // MAGIC + roots
        if file.read(&mut header)? == 0 {
            // New DB: Write header
            let mut writer = BufWriter::new(Vec::new());
            writer.write_all(&MAGIC)?;
            writer.write_i64::<LittleEndian>(-1)?; // index_root
            writer.write_i32::<LittleEndian>(0)?;
            writer.write_i64::<LittleEndian>(-1)?; // trie_root
            writer.write_i32::<LittleEndian>(0)?;
            writer.write_i64::<LittleEndian>(-1)?; // free_list_root
            writer.write_i32::<LittleEndian>(0)?;
            file.write_all(&writer.into_inner()?)?;
            file.flush()?;
        } else {
            let mut reader = Cursor::new(header);
            let magic = reader.read_u64::<LittleEndian>()?;
            if magic != u64::from_le_bytes(MAGIC) {
                return Err(io::Error::new(io::ErrorKind::InvalidData, "Invalid DB magic"));
            }
            *self.document_index_root.write() = VersionedLink {
                page_id: reader.read_i64::<LittleEndian>()?,
                version: reader.read_i32::<LittleEndian>()?,
            };
            *self.trie_root.write() = VersionedLink {
                page_id: reader.read_i64::<LittleEndian>()?,
                version: reader.read_i32::<LittleEndian>()?,
            };
            *self.free_list_root.write() = VersionedLink {
                page_id: reader.read_i64::<LittleEndian>()?,
                version: reader.read_i32::<LittleEndian>()?,
            };
        }
        self.recover()?;
        Ok(())
    }

    fn recover(&mut self) -> io::Result<()> {
        let mut used_pages = vec![];
        let mut index = BTreeMap::new();
        let mut trie = BTreeMap::new();
        let mut current_size = self.file.lock().metadata()?.len();
        let max_page_id = (current_size / self.config.page_size) as i64;

        for page_id in 0..max_page_id {
            let header = match self.read_page_header(page_id) {
                Ok(h) => h,
                Err(_) => continue,
            };
            if header.flags & FLAG_DATA_PAGE != 0 || header.flags & FLAG_TRIE_PAGE != 0 || header.flags & FLAG_INDEX_PAGE != 0 {
                used_pages.push(page_id);
                if header.flags & FLAG_INDEX_PAGE != 0 {
                    let data = self.read_raw_page(page_id)?;
                    let docs = self.deserialize_index(&data)?;
                    for (id, doc) in docs {
                        index.insert(id, doc);
                    }
                } else if header.flags & FLAG_TRIE_PAGE != 0 {
                    let node = self.deserialize_trie_node(&data)?;
                    trie.insert(page_id, node);
                }
            }
        }

        // Rebuild free list
        let mut free_pages = (0..max_page_id).filter(|&id| !used_pages.contains(&id)).collect::<Vec<_>>();
        free_pages.sort();
        let mut free_root = self.free_list_root.write();
        free_root.page_id = if !free_pages.is_empty() {
            let first_free = free_pages[0];
            self.write_free_list_page(first_free, &free_pages[1..], free_pages.len() as i32)?;
            first_free
        } else {
            -1
        };

        // Update index/trie roots
        if !index.is_empty() {
            let index_page = self.allocate_page()?;
            self.write_raw_page(index_page, &self.serialize_index(&index)?, 0)?;
            *self.document_index_root.write() = VersionedLink { page_id: index_page, version: 0 };
        }
        if !trie.is_empty() {
            let trie_page = self.allocate_page()?;
            let root_node = trie.remove(&trie_root.page_id).unwrap_or_default();
            self.write_raw_page(trie_page, &self.serialize_trie_node(&root_node)?, 0)?;
            *self.trie_root.write() = VersionedLink { page_id: trie_page, version: 0 };
        }

        *self.current_size.lock() = current_size;
        Ok(())
    }

    fn validate_path(&self, path: &str) -> io::Result<()> {
        if path.is_empty() || path.contains("..") || path.contains("::") || path.starts_with('/') || path.starts_with('\\') {
            return Err(io::Error::new(io::ErrorKind::InvalidInput, "Invalid path"));
        }
        Ok(())
    }

    fn read_raw_page(&self, page_id: i64) -> io::Result<Vec<u8>> {
        if page_id < 0 || page_id >= self.config.max_pages {
            return Err(io::Error::new(io::ErrorKind::InvalidInput, "Invalid page ID"));
        }
        if let Some(cached) = self.page_cache.lock().get(&page_id) {
            self.cache_stats.lock().hits += 1;
            return Ok(cached.clone());
        }
        self.cache_stats.lock().misses += 1;
        let offset = page_id as u64 * self.config.page_size + self.config.page_header_size;
        let header = self.read_page_header(page_id)?;
        let mut buffer = vec![0u8; header.data_length as usize];
        if let Some(mmap) = self.mmap.read().as_ref() {
            let start = offset as usize;
            buffer.copy_from_slice(&mmap[start..start + header.data_length as usize]);
        } else {
            let mut file = self.file.lock();
            file.seek(SeekFrom::Start(offset))?;
            file.read_exact(&mut buffer)?;
        }
        if !self.quick_mode.load(std::sync::atomic::Ordering::SeqCst) {
            let computed_crc = self.compute_crc(&buffer);
            if computed_crc != header.crc {
                return Err(io::Error::new(io::ErrorKind::InvalidData, "CRC mismatch"));
            }
        }
        let data = if self.config.use_compression {
            snappy::decompress(&buffer)?
        } else {
            buffer
        };
        self.page_cache.lock().put(page_id, data.clone());
        Ok(data)
    }

    fn write_raw_page(&self, page_id: i64, data: &[u8], version: i32) -> io::Result<()> {
        if page_id < 0 || page_id >= self.config.max_pages {
            return Err(io::Error::new(io::ErrorKind::InvalidInput, "Invalid page ID"));
        }
        let mut compressed = if self.config.use_compression {
            snappy::compress(data)
        } else {
            data.to_vec()
        };
        if compressed.len() as u64 > self.config.page_size - self.config.page_header_size {
            return Err(io::Error::new(io::ErrorKind::InvalidInput, "Data too large for page"));
        }
        let crc = self.compute_crc(&compressed);
        let header = PageHeader {
            crc,
            version,
            prev_page_id: -1,
            next_page_id: -1,
            flags: FLAG_DATA_PAGE,
            data_length: compressed.len() as i32,
            padding: [0; 3],
        };
        self.write_page_header(page_id, &header)?;
        let offset = page_id as u64 * self.config.page_size + self.config.page_header_size;
        if let Some(mmap) = self.mmap.write().as_mut() {
            let start = offset as usize;
            mmap[start..start + compressed.len()].copy_from_slice(&compressed);
            mmap.flush()?;
        } else {
            let mut file = self.file.lock();
            file.seek(SeekFrom::Start(offset))?;
            file.write_all(&compressed)?;
            file.flush()?;
        }
        self.page_cache.lock().pop(&page_id);
        Ok(())
    }

    fn write_page_header(&self, page_id: i64, header: &PageHeader) -> io::Result<()> {
        let offset = page_id as u64 * self.config.page_size;
        let mut buffer = Vec::new();
        let mut writer = BufWriter::new(&mut buffer);
        writer.write_u32::<LittleEndian>(header.crc)?;
        writer.write_i32::<LittleEndian>(header.version)?;
        writer.write_i64::<LittleEndian>(header.prev_page_id)?;
        writer.write_i64::<LittleEndian>(header.next_page_id)?;
        writer.write_u8(header.flags)?;
        writer.write_i32::<LittleEndian>(header.data_length)?;
        writer.write_all(&header.padding)?;
        let data = buffer;
        if let Some(mmap) = self.mmap.write().as_mut() {
            let start = offset as usize;
            mmap[start..start + self.config.page_header_size as usize].copy_from_slice(&data);
            mmap.flush()?;
        } else {
            let mut file = self.file.lock();
            file.seek(SeekFrom::Start(offset))?;
            file.write_all(&data)?;
            file.flush()?;
        }
        Ok(())
    }

    fn read_page_header(&self, page_id: i64) -> io::Result<PageHeader> {
        if page_id < 0 || page_id >= self.config.max_pages {
            return Err(io::Error::new(io::ErrorKind::InvalidInput, "Invalid page ID"));
        }
        let offset = page_id as u64 * self.config.page_size;
        let mut buffer = vec![0u8; self.config.page_header_size as usize];
        if let Some(mmap) = self.mmap.read().as_ref() {
            let start = offset as usize;
            buffer.copy_from_slice(&mmap[start..start + self.config.page_header_size as usize]);
        } else {
            let mut file = self.file.lock();
            file.seek(SeekFrom::Start(offset))?;
            file.read_exact(&mut buffer)?;
        }
        let mut reader = Cursor::new(buffer);
        let crc = reader.read_u32::<LittleEndian>()?;
        let version = reader.read_i32::<LittleEndian>()?;
        let prev_page_id = reader.read_i64::<LittleEndian>()?;
        let next_page_id = reader.read_i64::<LittleEndian>()?;
        let flags = reader.read_u8()?;
        let data_length = reader.read_i32::<LittleEndian>()?;
        let mut padding = [0u8; 3];
        reader.read_exact(&mut padding)?;
        Ok(PageHeader {
            crc,
            version,
            prev_page_id,
            next_page_id,
            flags,
            data_length,
            padding,
        })
    }

    fn allocate_page(&self) -> io::Result<i64> {
        if let Ok(page_id) = self.pop_free_page() {
            *self.empty_free_list_count.lock() = 0;
            return Ok(page_id);
        }
        let mut empty_count = self.empty_free_list_count.lock();
        *empty_count += 1;
        if *empty_count >= MAX_CONSECUTIVE_EMPTY_FREE_LIST {
            let new_pages = self.grow_file(BATCH_GROW_PAGES)?;
            *empty_count = 0;
            return Ok(new_pages);
        }
        let page_id = {
            let mut current_size = self.current_size.lock();
            let page_id = (*current_size / self.config.page_size) as i64;
            if page_id >= self.config.max_pages {
                return Err(io::Error::new(io::ErrorKind::Other, "Max pages exceeded"));
            }
            *current_size += self.config.page_size;
            page_id
        };
        let mut file = self.file.lock();
        file.set_len(page_id as u64 * self.config.page_size + self.config.page_size)?;
        Ok(page_id)
    }

    fn pop_free_page(&self) -> io::Result<i64> {
        let mut free_root = self.free_list_root.lock();
        if free_root.page_id == -1 {
            return Err(io::Error::new(io::ErrorKind::NotFound, "No free pages"));
        }
        let offset = free_root.page_id as u64 * self.config.page_size + self.config.page_header_size;
        let mut buffer = vec![0u8; FREE_LIST_HEADER_SIZE as usize + 8];
        if let Some(mmap) = self.mmap.read().as_ref() {
            let start = offset as usize;
            buffer.copy_from_slice(&mmap[start..start + FREE_LIST_HEADER_SIZE as usize + 8]);
        } else {
            let mut file = self.file.lock();
            file.seek(SeekFrom::Start(offset))?;
            file.read_exact(&mut buffer)?;
        }
        let mut reader = Cursor::new(buffer);
        let next_free_list_page = reader.read_i64::<LittleEndian>()?;
        let used_entries = reader.read_i32::<LittleEndian>()?;
        if used_entries <= 0 {
            free_root.page_id = next_free_list_page;
            return Err(io::Error::new(io::ErrorKind::NotFound, "No free pages in list"));
        }
        let page_id = reader.read_i64::<LittleEndian>()?;
        self.update_free_list_used(free_root.page_id, used_entries - 1)?;
        if used_entries == 1 {
            free_root.page_id = next_free_list_page;
        }
        Ok(page_id)
    }

    fn update_free_list_used(&self, page_id: i64, used_entries: i32) -> io::Result<()> {
        let offset = page_id as u64 * self.config.page_size + self.config.page_header_size;
        let mut buffer = Vec::new();
        let mut writer = BufWriter::new(&mut buffer);
        writer.write_i64::<LittleEndian>(-1)?;
        writer.write_i32::<LittleEndian>(used_entries)?;
        let data = buffer;
        if let Some(mmap) = self.mmap.write().as_mut() {
            let start = offset as usize;
            mmap[start..start + FREE_LIST_HEADER_SIZE as usize].copy_from_slice(&data);
            mmap.flush()?;
        } else {
            let mut file = self.file.lock();
            file.seek(SeekFrom::Start(offset))?;
            file.write_all(&data)?;
            file.flush()?;
        }
        Ok(())
    }

    fn grow_file(&self, num_pages: u64) -> io::Result<i64> {
        let mut current_size = self.current_size.lock();
        let new_size = *current_size + num_pages * self.config.page_size;
        if new_size / self.config.page_size as u64 > self.config.max_pages as u64 {
            return Err(io::Error::new(io::ErrorKind::Other, "Max pages exceeded"));
        }
        let mut file = self.file.lock();
        file.set_len(new_size)?;
        *current_size = new_size;
        Ok((new_size / self.config.page_size) as i64 - num_pages as i64)
    }

    fn serialize_index(&self, index: &BTreeMap<Uuid, Document>) -> io::Result<Vec<u8>> {
        let mut buffer = Vec::new();
        let mut writer = BufWriter::new(&mut buffer);
        writer.write_i32::<LittleEndian>(index.len() as i32)?;
        for (id, doc) in index {
            writer.write_all(id.as_bytes())?;
            writer.write_i64::<LittleEndian>(doc.first_page_id)?;
            writer.write_i32::<LittleEndian>(doc.current_version)?;
            writer.write_i32::<LittleEndian>(doc.paths.len() as i32)?;
            for path in &doc.paths {
                let bytes = path.as_bytes();
                writer.write_i32::<LittleEndian>(bytes.len() as i32)?;
                writer.write_all(bytes)?;
            }
        }
        Ok(buffer)
    }

    fn deserialize_index(&self, data: &[u8]) -> io::Result<BTreeMap<Uuid, Document>> {
        let mut index = BTreeMap::new();
        let mut reader = Cursor::new(data);
        let count = reader.read_i32::<LittleEndian>()?;
        for _ in 0..count {
            let mut id_bytes = [0u8; 16];
            reader.read_exact(&mut id_bytes)?;
            let id = Uuid::from_bytes(id_bytes);
            let first_page_id = reader.read_i64::<LittleEndian>()?;
            let current_version = reader.read_i32::<LittleEndian>()?;
            let path_count = reader.read_i32::<LittleEndian>()?;
            let mut paths = Vec::with_capacity(path_count as usize);
            for _ in 0..path_count {
                let len = reader.read_i32::<LittleEndian>()?;
                let mut path_bytes = vec![0u8; len as usize];
                reader.read_exact(&mut path_bytes)?;
                paths.push(String::from_utf8(path_bytes)?);
            }
            index.insert(id, Document { id, first_page_id, current_version, paths });
        }
        Ok(index)
    }

    fn serialize_trie_node(&self, node: &ReverseTrieNode) -> io::Result<Vec<u8>> {
        let mut buffer = Vec::new();
        let mut writer = BufWriter::new(&mut buffer);
        let edge_bytes = node.edge.as_bytes();
        writer.write_i32::<LittleEndian>(edge_bytes.len() as i32)?;
        writer.write_all(edge_bytes)?;
        writer.write_i64::<LittleEndian>(node.parent_page_id)?;
        writer.write_i64::<LittleEndian>(node.self_page_id)?;
        writer.write_i32::<LittleEndian>(if node.document_id.is_some() { 1 } else { 0 })?;
        if let Some(id) = node.document_id {
            writer.write_all(id.as_bytes())?;
        }
        writer.write_i32::<LittleEndian>(node.children.len() as i32)?;
        for (ch, child_id) in &node.children {
            writer.write_u8(*ch as u8)?;
            writer.write_i64::<LittleEndian>(*child_id)?;
        }
        Ok(buffer)
    }

    fn deserialize_trie_node(&self, data: &[u8]) -> io::Result<ReverseTrieNode> {
        let mut reader = Cursor::new(data);
        let edge_len = reader.read_i32::<LittleEndian>()?;
        let mut edge_bytes = vec![0u8; edge_len as usize];
        reader.read_exact(&mut edge_bytes)?;
        let edge = String::from_utf8(edge_bytes)?;
        let parent_page_id = reader.read_i64::<LittleEndian>()?;
        let self_page_id = reader.read_i64::<LittleEndian>()?;
        let has_doc = reader.read_i32::<LittleEndian>()?;
        let document_id = if has_doc != 0 {
            let mut id_bytes = [0u8; 16];
            reader.read_exact(&mut id_bytes)?;
            Some(Uuid::from_bytes(id_bytes))
        } else {
            None
        };
        let child_count = reader.read_i32::<LittleEndian>()?;
        let mut children = BTreeMap::new();
        for _ in 0..child_count {
            let ch = reader.read_u8()? as char;
            let child_id = reader.read_i64::<LittleEndian>()?;
            children.insert(ch, child_id);
        }
        Ok(ReverseTrieNode { edge, parent_page_id, self_page_id, document_id, children })
    }

    fn compute_crc(&self, data: &[u8]) -> u32 {
        let crc = Crc::<u32>::new(&CRC_32_ISO_HDLC);
        crc.checksum(data)
    }

    fn get_checksum(&self) -> u32 {
        let mut hasher = Md4::new();
        let mut header = vec![0u8; 32];
        self.file.lock().read_exact_at(&mut header, 0).unwrap_or(());
        hasher.update(&header);
        hasher.finalize().into()
    }

    fn write_document(&mut self, path: &CxxString, data: &CxxVector<u8>) -> io::Result<Uuid> {
        self.validate_path(path.to_string_lossy().as_ref())?;
        let id = Uuid::new_v4();
        let mut current_page_id = -1;
        let mut prev_page_id = -1;
        let mut data_remaining = data.as_slice();
        while !data_remaining.is_empty() {
            let chunk_size = std::cmp::min(data_remaining.len(), (self.config.page_size - self.config.page_header_size) as usize);
            let chunk = &data_remaining[..chunk_size];
            data_remaining = &data_remaining[chunk_size..];
            let new_page_id = self.allocate_page()?;
            let header = PageHeader {
                crc: self.compute_crc(chunk),
                version: 0,
                prev_page_id,
                next_page_id: if data_remaining.is_empty() { -1 } else { self.allocate_page()? },
                flags: FLAG_DATA_PAGE,
                data_length: chunk.len() as i32,
                padding: [0; 3],
            };
            self.write_raw_page(new_page_id, chunk, 0)?;
            self.write_page_header(new_page_id, &header)?;
            if current_page_id == -1 {
                current_page_id = new_page_id;
            }
            prev_page_id = new_page_id;
        }
        let mut index = self.read_index()?;
        let doc = Document {
            id,
            first_page_id: current_page_id,
            current_version: 0,
            paths: vec![path.to_string_lossy().to_string()],
        };
        index.insert(id, doc);
        let index_page = self.document_index_root.read().page_id;
        self.write_raw_page(index_page, &self.serialize_index(&index)?, self.document_index_root.read().version)?;
        self.trie_insert(path.to_string_lossy().as_ref(), id)?;
        Ok(id)
    }

    fn read_index(&self) -> io::Result<BTreeMap<Uuid, Document>> {
        let index_root = self.document_index_root.read();
        if index_root.page_id == -1 {
            return Ok(BTreeMap::new());
        }
        let data = self.read_raw_page(index_root.page_id)?;
        self.deserialize_index(&data)
    }

    fn get(&self, path: &CxxString) -> io::Result<CxxVector<u8>> {
        self.validate_path(path.to_string_lossy().as_ref())?;
        let id = self.get_document_id_by_path(path.to_string_lossy().as_ref())?;
        let index = self.read_index()?;
        let doc = index.get(&id).ok_or_else(|| io::Error::new(io::ErrorKind::NotFound, "Document not found"))?;
        let mut data = Vec::new();
        let mut current_page_id = doc.first_page_id;
        while current_page_id != -1 {
            let page_data = self.read_raw_page(current_page_id)?;
            data.extend_from_slice(&page_data);
            let header = self.read_page_header(current_page_id)?;
            current_page_id = header.next_page_id;
        }
        Ok(cxx::CxxVector::from(data))
    }

    fn search_paths(&self, prefix: &CxxString) -> io::Result<CxxVector<CxxString>> {
        self.validate_path(prefix.to_string_lossy().as_ref())?;
        let trie_root = self.trie_root.read();
        if trie_root.page_id == -1 {
            return Ok(cxx::CxxVector::new());
        }
        let reversed_prefix: String = prefix.to_string_lossy().chars().rev().collect();
        let mut current_page_id = trie_root.page_id;
        let mut remaining = reversed_prefix.as_str();
        let mut results = vec![];
        while !remaining.is_empty() {
            let node = self.deserialize_trie_node(&self.read_raw_page(current_page_id)?)?;
            let edge = node.edge.as_str();
            if remaining.starts_with(edge) {
                remaining = &remaining[edge.len()..];
                if remaining.is_empty() {
                    self.trie_collect_paths(&node, String::new(), &mut results)?;
                    let mut cxx_results = cxx::CxxVector::new();
                    for r in results.into_iter().filter(|p| p.starts_with(prefix.to_string_lossy().as_ref())) {
                        cxx_results.push(cxx::CxxString::from(r.as_str()));
                    }
                    return Ok(cxx_results);
                }
                let first_char = remaining.chars().next().unwrap();
                if let Some(&child_id) = node.children.get(&first_char) {
                    current_page_id = child_id;
                } else {
                    return Ok(cxx::CxxVector::new());
                }
            } else {
                return Ok(cxx::CxxVector::new());
            }
        }
        let node = self.deserialize_trie_node(&self.read_raw_page(current_page_id)?)?;
        self.trie_collect_paths(&node, String::new(), &mut results)?;
        let mut cxx_results = cxx::CxxVector::new();
        for r in results.into_iter().filter(|p| p.starts_with(prefix.to_string_lossy().as_ref())) {
            cxx_results.push(cxx::CxxString::from(r.as_str()));
        }
        Ok(cxx_results)
    }

    fn trie_collect_paths(&self, node: &ReverseTrieNode, prefix: String, results: &mut Vec<String>) -> io::Result<()> {
        let new_prefix = if prefix.is_empty() { node.edge.clone() } else { format!("{}{}", node.edge, prefix) };
        if let Some(id) = node.document_id {
            results.push(new_prefix.chars().rev().collect());
        }
        for &child_id in node.children.values() {
            let child = self.deserialize_trie_node(&self.read_raw_page(child_id)?)?;
            self.trie_collect_paths(&child, new_prefix.clone(), results)?;
        }
        Ok(())
    }

    fn trie_insert(&mut self, path: &str, id: Uuid) -> io::Result<()> {
        let reversed: String = path.chars().rev().collect();
        let mut current_page_id = self.trie_root.read().page_id;
        if current_page_id == -1 {
            current_page_id = self.allocate_page()?;
            *self.trie_root.write() = VersionedLink { page_id: current_page_id, version: 0 };
            self.write_raw_page(current_page_id, &self.serialize_trie_node(&ReverseTrieNode {
                edge: "".to_string(),
                parent_page_id: -1,
                self_page_id: current_page_id,
                document_id: None,
                children: BTreeMap::new(),
            })?, 0)?;
        }
        let mut remaining = reversed.as_str();
        while !remaining.is_empty() {
            let node = self.deserialize_trie_node(&self.read_raw_page(current_page_id)?)?;
            let edge = node.edge.as_str();
            let common_prefix = remaining.chars()
                .zip(edge.chars())
                .take_while(|(a, b)| a == b)
                .count();
            if common_prefix == edge.len() && common_prefix == remaining.len() {
                let mut new_node = node;
                new_node.document_id = Some(id);
                self.write_raw_page(current_page_id, &self.serialize_trie_node(&new_node)?, 0)?;
                return Ok(());
            }
            // Implement split/merge logic for trie (omitted for brevity)
            // ...
            return Ok(());
        }
        Ok(())
    }

    fn delete_by_path(self: Pin<&mut Self>, path: &CxxString) -> io::Result<()> {
        let rust_path = path.to_string_lossy().to_string();
        self.validate_path(&rust_path)?;
        let id = self.get_document_id_by_path(&rust_path)?;
        let mut index = self.read_index()?;
        let doc = index.remove(&id).ok_or_else(|| io::Error::new(io::ErrorKind::NotFound, "Document not found"))?;
        let mut current_page_id = doc.first_page_id;
        while current_page_id != -1 {
            let header = self.read_page_header(current_page_id)?;
            self.free_page(current_page_id)?;
            current_page_id = header.next_page_id;
        }
        for p in &doc.paths {
            self.trie_delete(p)?;
        }
        let index_page = self.document_index_root.read().page_id;
        self.write_raw_page(index_page, &self.serialize_index(&index)?, self.document_index_root.read().version)?;
        Ok(())
    }

    fn trie_delete(&mut self, path: &str) -> io::Result<()> {
        let reversed: String = path.chars().rev().collect();
        let mut current_page_id = self.trie_root.read().page_id;
        if current_page_id == -1 {
            return Err(io::Error::new(io::ErrorKind::NotFound, "Path not found"));
        }
        // Implement trie deletion with pruning (omitted for brevity)
        // ...
        Ok(())
    }

    fn get_document_id_by_path(&self, path: &str) -> io::Result<Uuid> {
        self.validate_path(path)?;
        let trie_root = self.trie_root.read();
        if trie_root.page_id == -1 {
            return Err(io::Error::new(io::ErrorKind::NotFound, "Path not found"));
        }
        let reversed: String = path.chars().rev().collect();
        let mut current_page_id = trie_root.page_id;
        let mut remaining = reversed.as_str();
        while !remaining.is_empty() {
            let node = self.deserialize_trie_node(&self.read_raw_page(current_page_id)?)?;
            let edge = node.edge.as_str();
            if remaining.starts_with(edge) {
                remaining = &remaining[edge.len()..];
                if remaining.is_empty() {
                    return node.document_id.ok_or_else(|| io::Error::new(io::ErrorKind::NotFound, "Path not found"));
                }
                let first_char = remaining.chars().next().unwrap();
                if let Some(&child_id) = node.children.get(&first_char) {
                    current_page_id = child_id;
                } else {
                    return Err(io::Error::new(io::ErrorKind::NotFound, "Path not found"));
                }
            } else {
                return Err(io::Error::new(io::ErrorKind::NotFound, "Path not found"));
            }
        }
        let node = self.deserialize_trie_node(&self.read_raw_page(current_page_id)?)?;
        node.document_id.ok_or_else(|| io::Error::new(io::ErrorKind::NotFound, "Path not found"))
    }

    fn start_stream(&self, path: &CxxString) -> io::Result<i64> {
        let id = self.get_document_id_by_path(path.to_string_lossy().as_ref())?;
        let index = self.read_index()?;
        let doc = index.get(&id).ok_or_else(|| io::Error::new(io::ErrorKind::NotFound, "Document not found"))?;
        Ok(doc.first_page_id)
    }

    fn next_stream_chunk(&self, stream_id: i64) -> io::Result<CxxVector<u8>> {
        if stream_id == -1 {
            return Err(io::Error::new(io::ErrorKind::NotFound, "Stream ended"));
        }
        let data = self.read_raw_page(stream_id)?;
        let header = self.read_page_header(stream_id)?;
        Ok(cxx::CxxVector::from(data))
    }

    fn end_stream(self: Pin<&mut Self>, stream_id: i64) {
        // No-op; idTech4 manages file closure
    }

    fn bind_addon_path(self: Pin<&mut Self>, path: &CxxString, addon: bool) -> io::Result<()> {
        let rust_path = path.to_string_lossy().to_string();
        self.validate_path(&rust_path)?;
        let id = self.get_document_id_by_path(&rust_path)?;
        let mut index = self.read_index()?;
        let doc = index.get_mut(&id).ok_or_else(|| io::Error::new(io::ErrorKind::NotFound, "Document not found"))?;
        doc.paths.push(rust_path.clone());
        let index_page = self.document_index_root.read().page_id;
        self.write_raw_page(index_page, &self.serialize_index(&index)?, self.document_index_root.read().version)?;
        self.trie_insert(&rust_path, id)?;
        Ok(())
    }

    fn begin_transaction(self: Pin<&mut Self>) -> io::Result<i64> {
        let tx_id = self.transactions.lock().len() as i64;
        self.transactions.lock().push(Transaction {
            writes: VecDeque::new(),
            frees: Vec::new(),
        });
        Ok(tx_id)
    }

    fn commit_transaction(self: Pin<&mut Self>, tx_id: i64) -> io::Result<()> {
        let mut txs = self.transactions.lock();
        if tx_id as usize >= txs.len() {
            return Err(io::Error::new(io::ErrorKind::InvalidInput, "Invalid transaction ID"));
        }
        let tx = txs.remove(tx_id as usize).unwrap();
        for (page_id, data, version) in tx.writes {
            self.write_raw_page(page_id, &data, version)?;
        }
        for page_id in tx.frees {
            self.free_page(page_id)?;
        }
        Ok(())
    }

    fn rollback_transaction(self: Pin<&mut Self>, tx_id: i64) -> io::Result<()> {
        let mut txs = self.transactions.lock();
        if tx_id as usize >= txs.len() {
            return Err(io::Error::new(io::ErrorKind::InvalidInput, "Invalid transaction ID"));
        }
        txs.remove(tx_id as usize);
        Ok(())
    }

    fn set_quick_mode(self: Pin<&mut Self>, enabled: bool) {
        self.quick_mode.store(enabled, std::sync::atomic::Ordering::SeqCst);
    }

    fn get_cache_stats(&self) -> CacheStats {
        self.cache_stats.lock().clone()
    }

    fn close_db(self: Pin<&mut Self>) {
        if let Some(mmap) = self.mmap.write().as_mut() {
            mmap.flush().unwrap_or(());
        }
        self.file.lock().flush().unwrap_or(());
    }
}

pub fn main() {} // Required for cxx::bridge
