#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include <fs/hfs.h>
#include <driver/block.h>
#include <mm/kheap.h>

#define LOG_MOD_NAME 	"HFS"
#define LOG_MOD_ENABLE  1
#include <log.h>

union block{
    struct superblock superblock;
    struct inode inodes[INODES_PER_BLOCK];
    uint32_t bitmap[FLAGS_PER_BLOCK];
    struct directory_block directory_block;
    uint8_t data[BLOCK_SIZE];
    uint16_t pointers[INODE_INDIRECT_POINTERS_PER_BLOCK];
};

static vnode_ops_t hfs_vnode_ops = {
	.open    = hfs_open,
	.close   = hfs_close,
	.read    = hfs_read,
	.write   = hfs_write,
	.readdir = NULL,
	.create  = hfs_create,
	.mkdir   = hfs_mkdir,
	.remove  = hfs_remove
};

fs_type_t hfs_fs_type = {
	.fs_name = "hfs",
	.vfs_ops = {
		.mount   = hfs_mount,
		.unmount = hfs_unmount
	}
};

// helpers
static int get_bit(uint32_t* bitmap, uint32_t bit_index){
    uint32_t word_index = bit_index / 32;
    uint32_t bit_offset = bit_index % 32;
    return (bitmap[word_index] >> bit_offset) & 1;
}

static void set_bit(uint32_t* bitmap, uint32_t bit_index){
    uint32_t word_index = bit_index / 32;
    uint32_t bit_offset = bit_index % 32;
    bitmap[word_index] |= (1U << bit_offset);
}

static void clear_bit(uint32_t* bitmap, uint32_t bit_index){
    uint32_t word_index = bit_index / 32;
    uint32_t bit_offset = bit_index % 32;
    bitmap[word_index] &= ~(1U << bit_offset);
}

static int32_t find_free_bit_from(uint32_t* bitmap, uint32_t start, uint32_t max_bits){
    for(uint32_t i = start; i < max_bits; i++){
        if(!get_bit(bitmap, i)){
            return (int32_t)i;
        }
    }
    return -1;
}

static void sync_bitmaps_from_disk(struct hfs_data* hfs){
    union block bmap_block;
    
    // reloading block bmap
    if(blkread(hfs->device, hfs->sb.s_block_bitmap, &bmap_block) == 0){
        memcpy(hfs->block_bitmap, bmap_block.bitmap, BLOCK_SIZE);
    }
    
    // reloading inode bmap
    if(blkread(hfs->device, hfs->sb.s_inode_bitmap, &bmap_block) == 0){
        memcpy(hfs->inode_bitmap, bmap_block.bitmap, BLOCK_SIZE);
    }
}

static int32_t alloc_block(struct hfs_data* hfs){
    // search from data blocks start. if not found, try reloading bmap from disk, check, write back
    int32_t block = find_free_bit_from(hfs->block_bitmap, hfs->sb.s_data_blocks_start, hfs->sb.s_blocks_count);
    if(block < 0){
        union block bmap_block;
        if(blkread(hfs->device, hfs->sb.s_block_bitmap, &bmap_block) == 0){
            memcpy(hfs->block_bitmap, bmap_block.bitmap, BLOCK_SIZE);
            block = find_free_bit_from(hfs->block_bitmap, hfs->sb.s_data_blocks_start, hfs->sb.s_blocks_count);
        }
    }
    
    if (block < 0){
        LOG_ERROR("no free blocks available\n");
        return -1;
    }

    set_bit(hfs->block_bitmap, block);
    union block bmap_block;
    memcpy(bmap_block.bitmap, hfs->block_bitmap, BLOCK_SIZE);
    if(blkwrite(hfs->device, hfs->sb.s_block_bitmap, &bmap_block) < 0){
        clear_bit(hfs->block_bitmap, block);
        return -1;
    }
    return block;
}

static void free_block(struct hfs_data* hfs, uint32_t block_num){
    // check
    if(block_num < hfs->sb.s_data_blocks_start || block_num >= hfs->sb.s_blocks_count){
        return;
    }
    clear_bit(hfs->block_bitmap, block_num);
    
    // write back
    union block bmap_block;
    memcpy(bmap_block.bitmap, hfs->block_bitmap, BLOCK_SIZE);
    blkwrite(hfs->device, hfs->sb.s_block_bitmap, &bmap_block);
}

static int32_t alloc_inode(struct hfs_data* hfs){
    int32_t inode_num = find_free_bit_from(hfs->inode_bitmap, 1, hfs->sb.s_inodes_count);
    if(inode_num < 0){
        union block ibmap_block;
        if(blkread(hfs->device, hfs->sb.s_inode_bitmap, &ibmap_block) == 0){
            memcpy(hfs->inode_bitmap, ibmap_block.bitmap, BLOCK_SIZE);
            inode_num = find_free_bit_from(hfs->inode_bitmap, 1, hfs->sb.s_inodes_count);
        }
    }
    
    if(inode_num < 0){
        // LOG_ERROR("no free inodes available\n");
        return -1;
    }

    set_bit(hfs->inode_bitmap, inode_num);
    union block ibmap_block;
    memcpy(ibmap_block.bitmap, hfs->inode_bitmap, BLOCK_SIZE);
    if(blkwrite(hfs->device, hfs->sb.s_inode_bitmap, &ibmap_block) < 0){
        clear_bit(hfs->inode_bitmap, inode_num);
        return -1;
    }
    return inode_num;
}

static void free_inode(struct hfs_data* hfs, uint32_t inode_num){
    // dont free root inode or invalid inodes
    if(inode_num == 0 || inode_num >= hfs->sb.s_inodes_count){
        return;
    }
    
    clear_bit(hfs->inode_bitmap, inode_num);

    union block ibmap_block;
    memcpy(ibmap_block.bitmap, hfs->inode_bitmap, BLOCK_SIZE);
    blkwrite(hfs->device, hfs->sb.s_inode_bitmap, &ibmap_block);
}

static int32_t read_inode(struct hfs_data* hfs, uint32_t inode_num, struct inode* inode_out){
    if(inode_num >= hfs->sb.s_inodes_count){
        return -1;
    }
    
    uint32_t block_index = inode_num / INODES_PER_BLOCK;
    uint32_t inode_offset = inode_num % INODES_PER_BLOCK;
    uint32_t block_num = hfs->sb.s_inode_table_block_start + block_index;
    
    union block blk;
    if(blkread(hfs->device, block_num, &blk) < 0){
        return -1;
    }
    
    memcpy(inode_out, &blk.inodes[inode_offset], sizeof(struct inode));
    return 0;
}

static int32_t write_inode(struct hfs_data* hfs, uint32_t inode_num, struct inode* inode_in){
    if(inode_num >= hfs->sb.s_inodes_count){
        return -1;
    }

    uint32_t block_index = inode_num / INODES_PER_BLOCK;
    uint32_t inode_offset = inode_num % INODES_PER_BLOCK;
    uint32_t block_num = hfs->sb.s_inode_table_block_start + block_index;

    union block blk;
    if(blkread(hfs->device, block_num, &blk) < 0){
        return -1;
    }
    memcpy(&blk.inodes[inode_offset], inode_in, sizeof(struct inode));
    if(blkwrite(hfs->device, block_num, &blk) < 0){
        return -1;
    }
    return 0;
}

static int32_t get_block_for_offset(struct hfs_data* hfs, struct inode* inode, uint32_t offset){
    uint32_t block_index = offset / BLOCK_SIZE;
    // check direct pointers first
    if(block_index < INODE_DIRECT_POINTERS){
        return inode->i_direct_pointers[block_index];
    }
    // use indirect pointer
    block_index -= INODE_DIRECT_POINTERS;
    if(inode->i_single_indirect_pointer == 0){
        return 0;
    }
    if(block_index >= INODE_INDIRECT_POINTERS_PER_BLOCK){
        return 0;
    }
    union block indirect_blk;
    if(blkread(hfs->device, inode->i_single_indirect_pointer, &indirect_blk) < 0){
        return -1;
    }
    return indirect_blk.pointers[block_index];
}

static int32_t alloc_block_for_offset(struct hfs_data* hfs, struct inode* inode, uint32_t offset){
    uint32_t block_index = offset / BLOCK_SIZE;
    int32_t new_block = alloc_block(hfs);
    if(new_block < 0){
        return -1;
    }
    
    union block zero_blk;
    memset(&zero_blk, 0, sizeof(union block));
    if(blkwrite(hfs->device, new_block, &zero_blk) < 0){
        free_block(hfs, new_block);
        return -1;
    }
    
    if(block_index < INODE_DIRECT_POINTERS){
        inode->i_direct_pointers[block_index] = new_block;
        return new_block;
    }
    
    block_index -= INODE_DIRECT_POINTERS;
    if(block_index >= INODE_INDIRECT_POINTERS_PER_BLOCK){
        free_block(hfs, new_block);
        return -1;
    }
    // if needed, allocate
    if(inode->i_single_indirect_pointer == 0){
        int32_t indirect_block = alloc_block(hfs);
        if(indirect_block < 0){
            free_block(hfs, new_block);
            return -1;
        }
        inode->i_single_indirect_pointer = indirect_block;
        memset(&zero_blk, 0, sizeof(union block));
        if(blkwrite(hfs->device, indirect_block, &zero_blk) < 0){
            free_block(hfs, indirect_block);
            free_block(hfs, new_block);
            inode->i_single_indirect_pointer = 0;
            return -1;
        }
    }
    union block indirect_blk;
    if(blkread(hfs->device, inode->i_single_indirect_pointer, &indirect_blk) < 0){
        free_block(hfs, new_block);
        return -1;
    }
    indirect_blk.pointers[block_index] = new_block;
    if(blkwrite(hfs->device, inode->i_single_indirect_pointer, &indirect_blk) < 0){
        free_block(hfs, new_block);
        return -1;
    }
    return new_block;
}

static const char* find_last_slash(const char* str){
    const char* last = NULL;
    while(*str){
        if(*str == '/'){
            last = str;
        }
        str++;
    }
    return last;
}

static void split_path(const char* path, char* parent, char* filename){
    const char* last_slash = find_last_slash(path);
    if(last_slash == NULL || last_slash == path){
        strcpy(parent, "/");
        strcpy(filename, last_slash ? last_slash + 1 : path);
    }
    else{
        size_t parent_len = last_slash - path;
        strncpy(parent, path, parent_len);
        parent[parent_len] = '\0';
        strcpy(filename, last_slash + 1);
    }
}

static int32_t find_dir_entry(struct hfs_data* hfs, struct inode* dir_inode, const char* name){
    if(!dir_inode->i_is_directory){
        return -1;
    }
    uint32_t num_blocks = (dir_inode->i_size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    for(uint32_t i = 0; i < num_blocks; i++){
        int32_t block_num = get_block_for_offset(hfs, dir_inode, i * BLOCK_SIZE);
        if(block_num <= 0){
            continue;
        }
        union block blk;
        if(blkread(hfs->device, block_num, &blk) < 0){
            continue;
        }
        for(uint32_t j = 0; j < DIRECTORY_ENTRIES_PER_BLOCK; j++){
            struct directory_entry* entry = &blk.directory_block.entries[j];
            if(entry->inode_number != 0 && strcmp(entry->name, name) == 0){
                return entry->inode_number;
            }
        }
    }
    return -1;
}

static int32_t add_dir_entry(struct hfs_data* hfs, struct inode* dir_inode, const char* name, uint32_t inode_num){
    if(!dir_inode->i_is_directory){
        return -1;
    }
    
    uint32_t num_blocks = (dir_inode->i_size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    if(num_blocks == 0){
        num_blocks = 1;
    }

    for(uint32_t i = 0; i < num_blocks; i++){
        int32_t block_num = get_block_for_offset(hfs, dir_inode, i * BLOCK_SIZE);
        if(block_num == 0){
            block_num = alloc_block_for_offset(hfs, dir_inode, i * BLOCK_SIZE);
            if (block_num < 0){
                return -1;
            }
        }
        union block blk;
        if(blkread(hfs->device, block_num, &blk) < 0){
            continue;
        }
        for(uint32_t j = 0; j < DIRECTORY_ENTRIES_PER_BLOCK; j++){
            if(blk.directory_block.entries[j].inode_number == 0){
                strncpy(blk.directory_block.entries[j].name, name, DIRECTORY_NAME_SIZE - 1);
                blk.directory_block.entries[j].name[DIRECTORY_NAME_SIZE - 1] = '\0';
                blk.directory_block.entries[j].inode_number = inode_num;
                if(blkwrite(hfs->device, block_num, &blk) < 0){
                    return -1;
                }
                uint32_t new_size = i * BLOCK_SIZE + (j + 1) * sizeof(struct directory_entry);
                if(dir_inode->i_size < new_size){
                    dir_inode->i_size = new_size;
                }
                return 0;
            }
        }
    }
    
    uint32_t new_block_offset = num_blocks * BLOCK_SIZE;
    int32_t new_block_num = alloc_block_for_offset(hfs, dir_inode, new_block_offset);
    if(new_block_num < 0){
        return -1;
    }
    
    union block blk;
    memset(&blk, 0, sizeof(union block));
    strncpy(blk.directory_block.entries[0].name, name, DIRECTORY_NAME_SIZE - 1);
    blk.directory_block.entries[0].name[DIRECTORY_NAME_SIZE - 1] = '\0';
    blk.directory_block.entries[0].inode_number = inode_num;
    
    if(blkwrite(hfs->device, new_block_num, &blk) < 0) {
        return -1;
    }
    
    dir_inode->i_size = new_block_offset + sizeof(struct directory_entry);
    return 0;
}

static int32_t remove_dir_entry(struct hfs_data* hfs, struct inode* dir_inode, const char* name){
    if(!dir_inode->i_is_directory){
        return -1;
    }
    
    uint32_t num_blocks = (dir_inode->i_size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    
    for(uint32_t i = 0; i < num_blocks; i++){
        int32_t block_num = get_block_for_offset(hfs, dir_inode, i * BLOCK_SIZE);
        if(block_num <= 0){
            continue;
        }
        union block blk;
        if(blkread(hfs->device, block_num, &blk) < 0){
            continue;
        }
        
        for(uint32_t j = 0; j < DIRECTORY_ENTRIES_PER_BLOCK; j++){
            if(blk.directory_block.entries[j].inode_number != 0 && strcmp(blk.directory_block.entries[j].name, name) == 0){
                memset(&blk.directory_block.entries[j], 0, sizeof(struct directory_entry));
                blkwrite(hfs->device, block_num, &blk);
                return 0;
            }
        }
    }
    return -1;
}

static int32_t resolve_path(struct hfs_data* hfs, const char* path){
    if(path == NULL || path[0] == '\0'){
        return -1;
    }
    if(strcmp(path, "/") == 0){
        return 0; // root always 0
    }
    
    char path_copy[256];
    strncpy(path_copy, path, sizeof(path_copy) - 1);
    path_copy[sizeof(path_copy) - 1] = '\0';

    char* p = path_copy;
    if (*p == '/'){
        p++;
    }
    uint32_t current_inode_num = 0;
    char* saveptr;
    char* token = strtok_r(p, "/", &saveptr);
    
    while(token != NULL){
        if(strlen(token) == 0){
            token = strtok_r(NULL, "/", &saveptr);
            continue;
        }
        
        struct inode current_inode;
        if(read_inode(hfs, current_inode_num, &current_inode) < 0){
            return -1;
        }
        if(!current_inode.i_is_directory){
            return -1;
        }
        
        int32_t next_inode = find_dir_entry(hfs, &current_inode, token);
        if(next_inode < 0){
            return -1;
        }
        current_inode_num = next_inode;
        token = strtok_r(NULL, "/", &saveptr);
    }
    return current_inode_num;
}

static void free_inode_blocks(struct hfs_data* hfs, struct inode* inode){
    for(int i = 0; i < INODE_DIRECT_POINTERS; i++){
        if(inode->i_direct_pointers[i] != 0){
            free_block(hfs, inode->i_direct_pointers[i]);
            inode->i_direct_pointers[i] = 0;
        }
    }
    
    if(inode->i_single_indirect_pointer != 0){
        union block indirect_blk;
        if(blkread(hfs->device, inode->i_single_indirect_pointer, &indirect_blk) == 0){
            for(uint32_t i = 0; i < INODE_INDIRECT_POINTERS_PER_BLOCK; i++){
                if(indirect_blk.pointers[i] != 0){
                    free_block(hfs, indirect_blk.pointers[i]);
                }
            }
        }
        free_block(hfs, inode->i_single_indirect_pointer);
        inode->i_single_indirect_pointer = 0;
    }
}

// api
int hfs_format(const char* device){
    block_device_t* dev = blkdev_get_by_name(device);
    if(!dev){
        LOG_ERROR("failed to get block device %s\n", device);
        return -1;
    }
    
    size_t block_size = blkdev_get_block_size(dev);
    size_t num_blocks = blkdev_get_num_blocks(dev);
    
    if(block_size != BLOCK_SIZE){
        LOG_ERROR("block size mismatch: expected %d, got %zu\n", BLOCK_SIZE, block_size);
        return -1;
    }
    
    uint32_t num_inodes = num_blocks / 4;
    if(num_inodes < 64){
        num_inodes = 64;
    }
    if(num_inodes > 4096){
        num_inodes = 4096;
    }
    uint32_t inode_blocks = (num_inodes * sizeof(struct inode) + BLOCK_SIZE - 1) / BLOCK_SIZE;
    // setup
    struct superblock sb;
    memset(&sb, 0, sizeof(sb));
    sb.s_magic = HFS_MAGIC;
    sb.s_blocks_count = num_blocks;
    sb.s_inodes_count = num_inodes;
    sb.s_block_bitmap = 1;
    sb.s_inode_bitmap = 2;
    sb.s_inode_table_block_start = 3;
    sb.s_data_blocks_start = 3 + inode_blocks;
    // superblock
    union block sb_block;
    memset(&sb_block, 0, sizeof(union block));
    memcpy(&sb_block.superblock, &sb, sizeof(struct superblock));
    if(blkwrite(dev, 0, &sb_block) < 0){
        LOG_ERROR("failed to write superblock\n");
        return -1;
    }
    
    // initialisa, clear, set, mark metadata blocks in bmap
    union block bmap_block;
    memset(&bmap_block, 0, sizeof(union block));
    for(uint32_t i = 0; i < sb.s_data_blocks_start; i++){
        set_bit(bmap_block.bitmap, i);
    }
    
    if(blkwrite(dev, sb.s_block_bitmap, &bmap_block) < 0){
        LOG_ERROR("failed to write block bitmap\n");
        return -1;
    }
    
    // init, clear, mark root inode in bmap
    union block ibmap_block;
    memset(&ibmap_block, 0, sizeof(union block));
    set_bit(ibmap_block.bitmap, 0);
    if(blkwrite(dev, sb.s_inode_bitmap, &ibmap_block) < 0){
        LOG_ERROR("failed to write inode bitmap\n");
        return -1;
    }
    
    // clr all
    union block zero_block;
    memset(&zero_block, 0, sizeof(union block));
    for(uint32_t i = 0; i < inode_blocks; i++){
        if(blkwrite(dev, sb.s_inode_table_block_start + i, &zero_block) < 0){
            LOG_ERROR("failed to clear inode table block %u\n", i);
            return -1;
        }
    }
    
    // prealloc for root
    uint32_t root_data_block = sb.s_data_blocks_start;
    set_bit(bmap_block.bitmap, root_data_block);
    
    if(blkwrite(dev, sb.s_block_bitmap, &bmap_block) < 0){
        LOG_ERROR("failed to update block bitmap\n");
        return -1;
    }
    if(blkwrite(dev, root_data_block, &zero_block) < 0){
        LOG_ERROR("failed to clear root data block\n");
        return -1;
    }
    // root
    struct inode root_inode;
    memset(&root_inode, 0, sizeof(struct inode));
    root_inode.i_is_directory = 1;
    root_inode.i_size = 0;
    root_inode.i_direct_pointers[0] = root_data_block;
    // write root
    union block inode_block;
    memset(&inode_block, 0, sizeof(union block));
    memcpy(&inode_block.inodes[0], &root_inode, sizeof(struct inode));
    if(blkwrite(dev, sb.s_inode_table_block_start, &inode_block) < 0){
        LOG_ERROR("failed to write root inode\n");
        return -1;
    }
    LOG_DEBUG("formatted device %s: %u blocks, %u inodes, data@%u\n", device, (unsigned)num_blocks, num_inodes, sb.s_data_blocks_start);
    
    // debugged: if filesystem already mounted, reload the bitmaps into memory
    vfs* mounted_fs = vfs_get_mounted("/test");
    if(mounted_fs != NULL){
        struct hfs_data* fs_data_mounted = (struct hfs_data*)mounted_fs->fs_data;
        if(fs_data_mounted != NULL){
            union block bitmap_reload;
            if(blkread(dev, sb.s_block_bitmap, &bitmap_reload) == 0){
                memcpy(fs_data_mounted->block_bitmap, bitmap_reload.bitmap, BLOCK_SIZE);
            }
            if(blkread(dev, sb.s_inode_bitmap, &bitmap_reload) == 0){
                memcpy(fs_data_mounted->inode_bitmap, bitmap_reload.bitmap, BLOCK_SIZE);
            }
            memcpy(&fs_data_mounted->sb, &sb, sizeof(struct superblock));
            
            // in case this changed
            fs_data_mounted->device = dev;
        }
    }
    return 0;
}

vfs* hfs_mount(const char* device){
    block_device_t* dev = blkdev_get_by_name(device);
    if(!dev){
        LOG_ERROR("failed to get block device %s\n", device);
        return NULL;
    }
    
    union block sb_block;
    if(blkread(dev, 0, &sb_block) < 0){
        LOG_ERROR("failed to read superblock\n");
        return NULL;
    }
    
    struct superblock* sb = &sb_block.superblock;
    if(sb->s_magic != HFS_MAGIC){
        LOG_ERROR("invalid magic number: 0x%x\n", sb->s_magic);
        return NULL;
    }
    
    struct hfs_data* hfs_data = malloc(sizeof(struct hfs_data));
    if(!hfs_data){
        LOG_ERROR("failed to allocate hfs_data\n");
        return NULL;
    }
    memset(hfs_data, 0, sizeof(struct hfs_data));
    hfs_data->device = dev;
    memcpy(&hfs_data->sb, sb, sizeof(struct superblock));
    hfs_data->block_bitmap = malloc(BLOCK_SIZE);
    if(!hfs_data->block_bitmap){
        free(hfs_data);
        LOG_ERROR("failed to allocate block bitmap\n");
        return NULL;
    }
    
    union block bmap_block;
    if(blkread(dev, sb->s_block_bitmap, &bmap_block) < 0){
        free(hfs_data->block_bitmap);
        free(hfs_data);
        LOG_ERROR("failed to read block bitmap\n");
        return NULL;
    }
    memcpy(hfs_data->block_bitmap, bmap_block.bitmap, BLOCK_SIZE);
    
    hfs_data->inode_bitmap = malloc(BLOCK_SIZE);
    if(!hfs_data->inode_bitmap){
        free(hfs_data->block_bitmap);
        free(hfs_data);
        LOG_ERROR("failed to allocate inode bitmap\n");
        return NULL;
    }
    
    union block ibmap_block;
    if(blkread(dev, sb->s_inode_bitmap, &ibmap_block) < 0){
        free(hfs_data->inode_bitmap);
        free(hfs_data->block_bitmap);
        free(hfs_data);
        LOG_ERROR("failed to read inode bitmap\n");
        return NULL;
    }
    memcpy(hfs_data->inode_bitmap, ibmap_block.bitmap, BLOCK_SIZE);
    
    // create root vnode
    vnode* root = malloc(sizeof(vnode));
    if(!root){
        free(hfs_data->inode_bitmap);
        free(hfs_data->block_bitmap);
        free(hfs_data);
        LOG_ERROR("failed to allocate root vnode\n");
        return NULL;
    }
    memset(root, 0, sizeof(vnode));
    strcpy(root->name, "/");
    root->type = V_DIRECTORY;
    root->ops = &hfs_vnode_ops;
    root->flags = 0;
    
    uint32_t* inode_num_ptr = malloc(sizeof(uint32_t));
    if(!inode_num_ptr){
        free(root);
        free(hfs_data->inode_bitmap);
        free(hfs_data->block_bitmap);
        free(hfs_data);
        LOG_ERROR("failed to allocate inode num ptr\n");
        return NULL;
    }
    *inode_num_ptr = 0;
    root->data = inode_num_ptr;
    
    // make vfs object
    vfs* filesystem = malloc(sizeof(vfs));
    if(!filesystem){
        free(inode_num_ptr);
        free(root);
        free(hfs_data->inode_bitmap);
        free(hfs_data->block_bitmap);
        free(hfs_data);
        LOG_ERROR("failed to allocate vfs\n");
        return NULL;
    }
    memset(filesystem, 0, sizeof(vfs));
    filesystem->type = &hfs_fs_type;
    filesystem->vroot = root;
    filesystem->fs_data = hfs_data;
    filesystem->vcovered = NULL;
    
    root->vfs_ptr = filesystem;
    LOG_DEBUG("mounted HFS from device %s\n", device);
    return filesystem;
}

int32_t hfs_unmount(vfs* fsys){
    if(!fsys){
        return -1;
    }
    
    struct hfs_data* hfs_data = (struct hfs_data*)fsys->fs_data;
    if(hfs_data){
        if(hfs_data->block_bitmap){
            free(hfs_data->block_bitmap);
            hfs_data->block_bitmap = NULL;
        }
        if(hfs_data->inode_bitmap){
            free(hfs_data->inode_bitmap);
            hfs_data->inode_bitmap = NULL;
        }
        free(hfs_data);
        fsys->fs_data = NULL;
    }
    
    if(fsys->vroot){
        if(fsys->vroot->data){
            free(fsys->vroot->data);
            fsys->vroot->data = NULL;
        }
        free(fsys->vroot);
        fsys->vroot = NULL;
    }
    free(fsys);
    LOG_DEBUG("unmounted HFS filesystem\n");
    return 0;
}

int32_t hfs_create(vnode* root, const char* path){
    if(!root || !path || !root->vfs_ptr || !root->vfs_ptr->fs_data){
        return -1;
    }
    
    struct hfs_data* hfs = (struct hfs_data*)root->vfs_ptr->fs_data;
    sync_bitmaps_from_disk(hfs);
    char parent_path[256], filename[DIRECTORY_NAME_SIZE];
    split_path(path, parent_path, filename);
    
    if(strlen(filename) == 0){
        return -1;
    }
    
    int32_t parent_inode_num = resolve_path(hfs, parent_path);
    if(parent_inode_num < 0){
        LOG_ERROR("parent directory not found: %s\n", parent_path);
        return -1;
    }
    struct inode parent_inode;
    if(read_inode(hfs, parent_inode_num, &parent_inode) < 0){
        return -1;
    }
    if(!parent_inode.i_is_directory){
        return -1;
    }
    if(find_dir_entry(hfs, &parent_inode, filename) >= 0){
        LOG_ERROR("file already exists: %s\n", filename);
        return -1;
    }
    int32_t new_inode_num = alloc_inode(hfs);
    if(new_inode_num < 0){
        return -1;
    }
    
    struct inode new_inode;
    memset(&new_inode, 0, sizeof(struct inode));
    new_inode.i_is_directory = 0;
    new_inode.i_size = 0;
    
    if(write_inode(hfs, new_inode_num, &new_inode) < 0){
        free_inode(hfs, new_inode_num);
        return -1;
    }
    
    if(add_dir_entry(hfs, &parent_inode, filename, new_inode_num) < 0){
        free_inode(hfs, new_inode_num);
        return -1;
    }
    if(write_inode(hfs, parent_inode_num, &parent_inode) < 0){
        return -1;
    }
    LOG_DEBUG("created file %s (inode %d)\n", path, new_inode_num);
    return 0;
}

int32_t hfs_mkdir(vnode* root, const char* path){
    if(!root || !path || !root->vfs_ptr || !root->vfs_ptr->fs_data){
        return -1;
    }

    struct hfs_data* hfs = (struct hfs_data*)root->vfs_ptr->fs_data;
    sync_bitmaps_from_disk(hfs);
    char parent_path[256], dirname[DIRECTORY_NAME_SIZE];
    split_path(path, parent_path, dirname);
    if(strlen(dirname) == 0){
        return -1;
    }
    int32_t parent_inode_num = resolve_path(hfs, parent_path);
    if(parent_inode_num < 0){
        LOG_ERROR("parent directory not found: %s\n", parent_path);
        return -1;
    }
    
    struct inode parent_inode;
    if(read_inode(hfs, parent_inode_num, &parent_inode) < 0){
        return -1;
    }
    if(!parent_inode.i_is_directory){
        return -1;
    }
    if(find_dir_entry(hfs, &parent_inode, dirname) >= 0){
        LOG_ERROR("directory already exists: %s\n", dirname);
        return -1;
    }
    int32_t new_inode_num = alloc_inode(hfs);
    if(new_inode_num < 0){
        return -1;
    }
    struct inode new_inode;
    memset(&new_inode, 0, sizeof(struct inode));
    new_inode.i_is_directory = 1;
    new_inode.i_size = 0;
    if(write_inode(hfs, new_inode_num, &new_inode) < 0){
        free_inode(hfs, new_inode_num);
        return -1;
    }
    if(add_dir_entry(hfs, &parent_inode, dirname, new_inode_num) < 0){
        free_inode(hfs, new_inode_num);
        return -1;
    }
    if(write_inode(hfs, parent_inode_num, &parent_inode) < 0){
        return -1;
    }
    LOG_DEBUG("created directory %s (inode %d)\n", path, new_inode_num);
    return 0;
}

int32_t hfs_remove(vnode* root, const char* path){
    if(!root || !path || !root->vfs_ptr || !root->vfs_ptr->fs_data){
        return -1;
    }
    
    struct hfs_data* hfs = (struct hfs_data*)root->vfs_ptr->fs_data;
    int32_t inode_num = resolve_path(hfs, path);
    if(inode_num < 0){
        LOG_ERROR("path not found: %s\n", path);
        return -1;
    }
    if(inode_num == 0){
        return -1;
    }
    
    struct inode target_inode;
    if(read_inode(hfs, inode_num, &target_inode) < 0) {
        return -1;
    }
    
    if(target_inode.i_is_directory){
        uint32_t num_blocks = (target_inode.i_size + BLOCK_SIZE - 1) / BLOCK_SIZE;
        for(uint32_t i = 0; i < num_blocks; i++){
            int32_t block_num = get_block_for_offset(hfs, &target_inode, i * BLOCK_SIZE);
            if(block_num <= 0){
                continue;
            }
            union block blk;
            if(blkread(hfs->device, block_num, &blk) < 0){
                continue;
            }
            for(uint32_t j = 0; j < DIRECTORY_ENTRIES_PER_BLOCK; j++){
                struct directory_entry* entry = &blk.directory_block.entries[j];
                if(entry->inode_number != 0){
                    char child_path[512];
                    size_t path_len = strlen(path);
                    size_t name_len = strlen(entry->name);
                    if(path_len + 1 + name_len + 1 < sizeof(child_path)){
                        strcpy(child_path, path);
                        child_path[path_len] = '/';
                        strcpy(child_path + path_len + 1, entry->name);
                        hfs_remove(root, child_path);
                    }
                }
            }
        }
    }
    free_inode_blocks(hfs, &target_inode);
    free_inode(hfs, inode_num);
    char parent_path[256], filename[DIRECTORY_NAME_SIZE];
    split_path(path, parent_path, filename);

    int32_t parent_inode_num = resolve_path(hfs, parent_path);
    if(parent_inode_num >= 0){
        struct inode parent_inode;
        if(read_inode(hfs, parent_inode_num, &parent_inode) == 0){
            remove_dir_entry(hfs, &parent_inode, filename);
            write_inode(hfs, parent_inode_num, &parent_inode);
        }
    }
    LOG_DEBUG("removed %s (inode %d)\n", path, inode_num);
    return 0;
}

vnode* hfs_open(vnode* root, const char* path, uint32_t flags){
    if(!root || !path || !root->vfs_ptr || !root->vfs_ptr->fs_data){
        return NULL;
    }
    
    struct hfs_data* hfs = (struct hfs_data*)root->vfs_ptr->fs_data;
    int32_t inode_num = resolve_path(hfs, path);
    if(inode_num < 0){
        LOG_ERROR("path not found: %s\n", path);
        return NULL;
    }
    
    struct inode inode;
    if(read_inode(hfs, inode_num, &inode) < 0){
        return NULL;
    }
    
    vnode* node = malloc(sizeof(vnode));
    if(!node){
        return NULL;
    }
    memset(node, 0, sizeof(vnode));
    const char* filename = find_last_slash(path);
    if(filename){
        filename++;
    }
    else{
        filename = path;
    }
    
    strncpy(node->name, filename, sizeof(node->name) - 1);
    node->name[sizeof(node->name) - 1] = '\0';
    node->type = inode.i_is_directory ? V_DIRECTORY : V_FILE;
    node->ops = &hfs_vnode_ops;
    node->vfs_ptr = root->vfs_ptr;
    node->flags = flags;
    
    uint32_t* inode_num_ptr = malloc(sizeof(uint32_t));
    if(!inode_num_ptr){
        free(node);
        return NULL;
    }
    *inode_num_ptr = inode_num;
    node->data = inode_num_ptr;
    LOG_DEBUG("opened %s (inode %d)\n", path, inode_num);
    return node;
}

int32_t hfs_close(vnode* node){
    if(!node){
        return -1;
    }
    if(node->data){
        free(node->data);
        node->data = NULL;
    }
    free(node);
    return 0;
}

int32_t hfs_read(vnode* node, uint32_t offset, uint32_t size, void* buf){
    if(!node || !buf || !node->data || !node->vfs_ptr || !node->vfs_ptr->fs_data){
        return -1;
    }
    
    struct hfs_data* hfs = (struct hfs_data*)node->vfs_ptr->fs_data;
    uint32_t inode_num = *(uint32_t*)node->data;
    struct inode inode;
    if(read_inode(hfs, inode_num, &inode) < 0){
        return -1;
    }
    if(offset >= inode.i_size){
        return 0;
    }
    
    if(offset + size > inode.i_size) {
        size = inode.i_size - offset;
    }
    
    uint32_t bytes_read = 0;
    uint8_t* buffer = (uint8_t*)buf;
    while(bytes_read < size){
        uint32_t current_offset = offset + bytes_read;
        uint32_t block_offset = current_offset % BLOCK_SIZE;
        uint32_t bytes_to_read = BLOCK_SIZE - block_offset;
        
        if(bytes_to_read > size - bytes_read){
            bytes_to_read = size - bytes_read;
        }
        
        int32_t block_num = get_block_for_offset(hfs, &inode, current_offset);
        if(block_num <= 0){
            memset(buffer + bytes_read, 0, bytes_to_read);
        }
        else{
            union block blk;
            if(blkread(hfs->device, block_num, &blk) < 0){
                return bytes_read > 0 ? (int32_t)bytes_read : -1;
            }
            memcpy(buffer + bytes_read, blk.data + block_offset, bytes_to_read);
        }
        bytes_read += bytes_to_read;
    }
    return (int32_t)bytes_read;
}

int32_t hfs_write(vnode* node, uint32_t offset, uint32_t size, void* buf){
    if(!node || !buf || !node->data || !node->vfs_ptr || !node->vfs_ptr->fs_data){
        return -1;
    }
    
    struct hfs_data* hfs = (struct hfs_data*)node->vfs_ptr->fs_data;
    sync_bitmaps_from_disk(hfs);
    uint32_t inode_num = *(uint32_t*)node->data;
    struct inode inode;
    if(read_inode(hfs, inode_num, &inode) < 0){
        return -1;
    }
    
    uint32_t bytes_written = 0;
    uint8_t* buffer = (uint8_t*)buf;
    while(bytes_written < size){
        uint32_t current_offset = offset + bytes_written;
        uint32_t block_offset = current_offset % BLOCK_SIZE;
        uint32_t bytes_to_write = BLOCK_SIZE - block_offset;
        
        if(bytes_to_write > size - bytes_written){
            bytes_to_write = size - bytes_written;
        }
        
        int32_t block_num = get_block_for_offset(hfs, &inode, current_offset);
        if(block_num == 0){
            block_num = alloc_block_for_offset(hfs, &inode, current_offset);
            if(block_num < 0){
                if(bytes_written > 0 && offset + bytes_written > inode.i_size){
                    inode.i_size = offset + bytes_written;
                }
                write_inode(hfs, inode_num, &inode);
                return bytes_written > 0 ? (int32_t)bytes_written : -1;
            }
            write_inode(hfs, inode_num, &inode);
        }
        
        union block blk;
        if(block_offset != 0 || bytes_to_write != BLOCK_SIZE){
            if(blkread(hfs->device, block_num, &blk) < 0){
                if(bytes_written > 0 && offset + bytes_written > inode.i_size){
                    inode.i_size = offset + bytes_written;
                }
                write_inode(hfs, inode_num, &inode);
                return bytes_written > 0 ? (int32_t)bytes_written : -1;
            }
        }
        else{
            memset(&blk, 0, sizeof(union block));
        }
        memcpy(blk.data + block_offset, buffer + bytes_written, bytes_to_write);
        
        if(blkwrite(hfs->device, block_num, &blk) < 0){
            if(bytes_written > 0 && offset + bytes_written > inode.i_size){
                inode.i_size = offset + bytes_written;
            }
            write_inode(hfs, inode_num, &inode);
            return bytes_written > 0 ? (int32_t)bytes_written : -1;
        }
        bytes_written += bytes_to_write;
    }
    
    if(offset + bytes_written > inode.i_size){
        inode.i_size = offset + bytes_written;
    }
    write_inode(hfs, inode_num, &inode);
    return (int32_t)bytes_written;
}

int32_t hfs_readdir(vnode* node, uint32_t index, struct directory_entry* entry){
    if(!node || !node->data || !entry || !node->vfs_ptr || !node->vfs_ptr->fs_data){
        return -1;
    }
    
    struct hfs_data* hfs = (struct hfs_data*)node->vfs_ptr->fs_data;
    uint32_t inode_num = *(uint32_t*)node->data;
    struct inode inode;
    if(read_inode(hfs, inode_num, &inode) < 0){
        return -1;
    }
    if(!inode.i_is_directory){
        return -1;
    }
    
    uint32_t current_index = 0;
    uint32_t num_blocks = (inode.i_size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    for(uint32_t i = 0; i < num_blocks; i++){
        int32_t block_num = get_block_for_offset(hfs, &inode, i * BLOCK_SIZE);
        if(block_num <= 0){
            continue;
        }
        union block blk;
        if(blkread(hfs->device, block_num, &blk) < 0){
            continue;
        }
        for(uint32_t j = 0; j < DIRECTORY_ENTRIES_PER_BLOCK; j++){
            if(blk.directory_block.entries[j].inode_number != 0){
                if(current_index == index){
                    memcpy(entry, &blk.directory_block.entries[j], sizeof(struct directory_entry));
                    return 0;
                }
                current_index++;
            }
        }
    }
    return -1;
}

// debugging
int32_t fs_list(vfs* fsys, const char* path){
    if(!fsys || !path || !fsys->fs_data){
        return -1;
    }
    
    struct hfs_data* hfs = (struct hfs_data*)fsys->fs_data;
    int32_t inode_num = resolve_path(hfs, path);
    if(inode_num < 0){
        return -1;
    }
    
    struct inode inode;
    if(read_inode(hfs, inode_num, &inode) < 0){
        return -1;
    }
    
    if(!inode.i_is_directory){
        return -1;
    }
    
    uint32_t num_blocks = (inode.i_size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    for(uint32_t i = 0; i < num_blocks; i++){
        int32_t block_num = get_block_for_offset(hfs, &inode, i * BLOCK_SIZE);
        if (block_num <= 0){
            continue;
        }
        union block blk;
        if(blkread(hfs->device, block_num, &blk) < 0){
            continue;
        }
        for(uint32_t j = 0; j < DIRECTORY_ENTRIES_PER_BLOCK; j++){
            struct directory_entry* entry = &blk.directory_block.entries[j];
            if(entry->inode_number != 0){
                struct inode entry_inode;
                if(read_inode(hfs, entry->inode_number, &entry_inode) == 0){
                    LOG_DEBUG("  %s (%u bytes, %s)\n", entry->name, entry_inode.i_size,entry_inode.i_is_directory ? "DIR" : "FILE");
                }
            }
        }
    }
    return 0;
}

int32_t fs_stat_file(vfs* fsys, const char* path){
    if(!fsys || !path || !fsys->fs_data){
        return -1;
    }
    
    struct hfs_data* hfs = (struct hfs_data*)fsys->fs_data;
    int32_t inode_num = resolve_path(hfs, path);
    if(inode_num < 0){
        return -1;
    }
    
    struct inode inode;
    if(read_inode(hfs, inode_num, &inode) < 0){
        return -1;
    }
    LOG_DEBUG("File: %s, Inode: %d, Type: %s, Size: %u\n", path, inode_num, inode.i_is_directory ? "Directory" : "File",inode.i_size);
    return 0;
}

void fs_stat(vfs* fsys){
    if(!fsys || !fsys->fs_data){
        return;
    }
    
    struct hfs_data* hfs = (struct hfs_data*)fsys->fs_data;
    uint32_t free_blocks = 0;
    for(uint32_t i = hfs->sb.s_data_blocks_start; i < hfs->sb.s_blocks_count; i++){
        if(!get_bit(hfs->block_bitmap, i)) {
            free_blocks++;
        }
    }
    
    uint32_t free_inodes = 0;
    for(uint32_t i = 1; i < hfs->sb.s_inodes_count; i++){
        if(!get_bit(hfs->inode_bitmap, i)){
            free_inodes++;
        }
    }
    LOG_DEBUG("HFS Stats: %u/%u blocks free, %u/%u inodes free\n",free_blocks, hfs->sb.s_blocks_count - hfs->sb.s_data_blocks_start,free_inodes, hfs->sb.s_inodes_count - 1);
}