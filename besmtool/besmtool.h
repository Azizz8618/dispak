#define ZBYTES  6144    /* size of zone in bytes */
#define MAXZ    010000  /* max size of disk is about 25 Mbytes */

void list_all_disks (void);
void list_disk (unsigned diskno);
void passports (unsigned diskno, unsigned start);
void arfa_catalog (unsigned diskno);
int arfa_format (unsigned diskno, const char *volabel);
int arfa_create (unsigned diskno, const char *name, unsigned start,
        unsigned nzones, unsigned attr, int special);
int arfa_destroy (unsigned diskno, const char *name);
void erase_disk (unsigned diskno, unsigned start, unsigned length, int format);
void view_disk (unsigned diskno, unsigned start, unsigned length, char *encoding);
void dump_disk (unsigned diskno, unsigned start, unsigned length);
void check_disk (unsigned diskno, unsigned start, unsigned length);
void search_disk (unsigned diskno, unsigned char *pattern, unsigned start, unsigned length);
void file_to_disk (unsigned to_diskno, unsigned to_start, unsigned length,
        char *from_file, unsigned from_start);
void dir_to_disk (unsigned to_diskno, char *from_dir);
void disk_to_disk (unsigned to_diskno, unsigned to_start, unsigned length,
        unsigned from_diskno, unsigned from_start);
void disk_to_file (unsigned from_diskno, unsigned from_start, unsigned length,
        char *to_file);
