/* deviceHandler-wiikeyfusion.c
	- device implementation for Wiikey Fusion (FAT filesystem)
	by emu_kidid
 */

#include <fat.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>
#include <ogc/dvd.h>
#include <sys/dir.h>
#include <sys/statvfs.h>
#include <ogc/machine/processor.h>
#include <sdcard/gcsd.h>
#include "deviceHandler.h"
#include "gui/FrameBufferMagic.h"
#include "gui/IPLFontWrite.h"
#include "swiss.h"
#include "main.h"
#include "wkf.h"
#include "frag.h"
#include "patcher.h"

const DISC_INTERFACE* wkf = &__io_wkf;
int wkfFragSetupReq = 0;

file_handle initial_WKF =
	{ "wkf:/",       // directory
	  0ULL,     // fileBase (u64)
	  0,        // offset
	  0,        // size
	  IS_DIR,
	  0,
	  0
	};


device_info initial_WKF_info = {
	0,
	0
};
	
device_info* deviceHandler_WKF_info() {
	return &initial_WKF_info;
}

s32 deviceHandler_WKF_readDir(file_handle* ffile, file_handle** dir, u32 type){	

	DIR* dp = opendir( ffile->name );
	if(!dp) return -1;
	struct dirent *entry;
	struct stat fstat;
	
	// Set everything up to read
	int num_entries = 1, i = 1;
	char file_name[1024];
	*dir = malloc( num_entries * sizeof(file_handle) );
	memset(*dir,0,sizeof(file_handle) * num_entries);
	(*dir)[0].fileAttrib = IS_SPECIAL;
	strcpy((*dir)[0].name, "..");
	
	// Read each entry of the directory
	while( (entry = readdir(dp)) != NULL ){
		if(strlen(entry->d_name) <= 2  && (entry->d_name[0] == '.' || entry->d_name[1] == '.')) {
			continue;
		}
		memset(&file_name[0],0,1024);
		sprintf(&file_name[0], "%s/%s", ffile->name, entry->d_name);
		stat(&file_name[0],&fstat);
		// Do we want this one?
		if(type == -1 || ((fstat.st_mode & S_IFDIR) ? (type==IS_DIR) : (type==IS_FILE))) {
			if(!(fstat.st_mode & S_IFDIR)) {
				if(!checkExtension(entry->d_name)) continue;
			}
			// Make sure we have room for this one
			if(i == num_entries){
				++num_entries;
				*dir = realloc( *dir, num_entries * sizeof(file_handle) ); 
			}
			memset(&(*dir)[i], 0, sizeof(file_handle));
			sprintf((*dir)[i].name, "%s/%s", ffile->name, entry->d_name);
			(*dir)[i].size     = fstat.st_size;
			(*dir)[i].fileAttrib   = (fstat.st_mode & S_IFDIR) ? IS_DIR : IS_FILE;
			if((*dir)[i].fileAttrib == IS_FILE) {
				get_frag_list((*dir)[i].name);
				u32 file_base = frag_list->num > 1 ? -1 : frag_list->frag[0].sector;
				(*dir)[i].fileBase = file_base;
			}
			else {
				(*dir)[i].fileBase = 0;
			}		
			++i;
		}
	}
	
	closedir(dp);
	return num_entries;
}


s32 deviceHandler_WKF_seekFile(file_handle* file, u32 where, u32 type){
	if(type == DEVICE_HANDLER_SEEK_SET) file->offset = where;
	else if(type == DEVICE_HANDLER_SEEK_CUR) file->offset += where;
	return file->offset;
}


s32 deviceHandler_WKF_readFile(file_handle* file, void* buffer, u32 length){
	if(!file->fp) {
		file->fp = fopen( file->name, "rb" );
		if(file->size <= 0) {
			struct stat fstat;
			stat(file->name,&fstat);
			file->size = fstat.st_size;
		}
	}
	if(!file->fp) return -1;
	
	fseek(file->fp, file->offset, SEEK_SET);
	int bytes_read = fread(buffer, 1, length, file->fp);
	if(bytes_read > 0) file->offset += bytes_read;
	return bytes_read;
}


s32 deviceHandler_WKF_writeFile(file_handle* file, void* buffer, u32 length){
	return -1;
}


s32 deviceHandler_WKF_setupFile(file_handle* file, file_handle* file2) {
	
	
	int maxFrags = (VAR_FRAG_SIZE/12), i = 0;
	u32 *fragList = (u32*)VAR_FRAG_LIST;
	int patches = 0;
	
	memset((void*)VAR_FRAG_LIST, 0, VAR_FRAG_SIZE);

	// Check if there are any fragments in our patch location for this game
	if(devices[DEVICE_PATCHES] != NULL) {
		print_gecko("Save Patch device found\r\n");
		
		// Look for patch files, if we find some, open them and add them as fragments
		file_handle patchFile;
		char gameID[8];
		memset(&gameID, 0, 8);
		strncpy((char*)&gameID, (char*)&GCMDisk, 4);
		
		for(i = 0; i < maxFrags; i++) {
			u32 patchInfo[4];
			patchInfo[0] = 0; patchInfo[1] = 0; 
			memset(&patchFile, 0, sizeof(file_handle));
			sprintf(&patchFile.name[0], "%sswiss_patches/%s/%i",devices[DEVICE_PATCHES]->initial->name,gameID, i);
			print_gecko("Looking for file %s\r\n", &patchFile.name);
			struct stat fstat;
			if(stat(&patchFile.name[0],&fstat)) {
				break;
			}
			patchFile.fp = fopen(&patchFile.name[0], "rb");
			if(patchFile.fp) {
				fseek(patchFile.fp, fstat.st_size-16, SEEK_SET);
				
				if((fread(&patchInfo, 1, 16, patchFile.fp) == 16) && (patchInfo[2] == SWISS_MAGIC)) {
					get_frag_list(&patchFile.name[0]);
					print_gecko("Found patch file %i ofs 0x%08X len 0x%08X base 0x%08X\r\n", 
									i, patchInfo[0], patchInfo[1], frag_list->frag[0].sector);
					fclose(patchFile.fp);
					fragList[patches*3] = patchInfo[0];
					fragList[(patches*3)+1] = patchInfo[1] | 0x80000000;
					fragList[(patches*3)+2] = frag_list->frag[0].sector;
					patches++;
				}
				else {
					break;
				}
			}
		}
		// Copy the current speed
		*(volatile unsigned int*)VAR_EXI_BUS_SPD = 192;
		// Card Type
		*(volatile unsigned int*)VAR_SD_TYPE = sdgecko_getAddressingType(((devices[DEVICE_PATCHES]->location == LOC_MEMCARD_SLOT_A) ? 0:1));
		// Copy the actual freq
		*(volatile unsigned int*)VAR_EXI_FREQ = EXI_SPEED16MHZ;	// play it safe
		// Device slot (0 or 1) // This represents 0xCC0068xx in number of u32's so, slot A = 0xCC006800, B = 0xCC006814
		*(volatile unsigned int*)VAR_EXI_SLOT = ((devices[DEVICE_PATCHES]->location == LOC_MEMCARD_SLOT_A) ? 0:1) * 5;
	}
	
	// Check if file2 exists
	if(file2) {
		get_frag_list(file2->name);
		if(frag_list->num <= 0)
			file2 = NULL;
	}

	// If there are 2 discs, we only allow 5 fragments per disc.
	int frags = patches;
	maxFrags = file2 ? ((VAR_FRAG_SIZE/12)/2) : (VAR_FRAG_SIZE/12);
	
	// If disc 1 is fragmented, make a note of the fragments and their sizes
	get_frag_list(file->name);
	if(frag_list->num < maxFrags) {
		for(i = 0; i < frag_list->num; i++) {
			fragList[frags*3] = frag_list->frag[i].offset*512;
			fragList[(frags*3)+1] = frag_list->frag[i].count*512;
			fragList[(frags*3)+2] = frag_list->frag[i].sector;
			//print_gecko("Wrote Frag: ofs: %08X count: %08X sector: %08X\r\n",
			//			fragList[frags*3],fragList[(frags*3)+1],fragList[(frags*3)+2]);
			frags++;
		}
	}
	else {
		// file is too fragmented - go defrag it!
		return 0;
	}
		
	// If there is a disc 2 and it's fragmented, make a note of the fragments and their sizes
	if(file2) {
		// No fragment room left for the second disc, fail.
		if(frags+1 == maxFrags) {
			return 0;
		}
		get_frag_list(file2->name);
		if(frag_list->num < maxFrags) {
			for(i = 0; i < frag_list->num; i++) {
				fragList[(frags*3) + (maxFrags*3)] = frag_list->frag[i].offset*512;
				fragList[((frags*3) + 1) + (maxFrags*3)]  = frag_list->frag[i].count*512;
				fragList[((frags*3) + 2) + (maxFrags*3)] = frag_list->frag[i].sector;
				//print_gecko("Wrote Frag: ofs: %08X count: %08X sector: %08X\r\n",
				//		fragList[frags*3],fragList[(frags*3)+1],fragList[(frags*3)+2]);
				frags++;
			}
		}
		else {
			// file is too fragmented - go defrag it!
			return 0;
		}
	}
	
	// Disk 1 base sector
	*(volatile unsigned int*)VAR_DISC_1_LBA = fragList[2];
	// Disk 2 base sector
	*(volatile unsigned int*)VAR_DISC_2_LBA = file2 ? fragList[2 + (maxFrags*3)]:fragList[2];
	// Currently selected disk base sector
	*(volatile unsigned int*)VAR_CUR_DISC_LBA = fragList[2];
	
	wkfFragSetupReq = (file2 && frags > 2) ? 1 : frags>1;
	print_frag_list(file2 != 0);
	return 1;
}

s32 deviceHandler_WKF_init(file_handle* file){
	struct statvfs buf;
	
	wkfReinit();
	int ret = fatMountSimple ("wkf", wkf) ? 1 : 0;
	initial_WKF_info.freeSpaceInKB = initial_WKF_info.totalSpaceInKB = 0;	
	if(ret) {
		if(deviceHandler_getStatEnabled()) {
			memset(&buf, 0, sizeof(statvfs));
			//DrawFrameStart();
			//DrawMessageBox(D_INFO,"Reading filesystem info for wkf:/");
			//DrawFrameFinish();
			
			int res = statvfs("wkf:/", &buf);
			initial_WKF_info.freeSpaceInKB = !res ? (u32)((uint64_t)((uint64_t)buf.f_bsize*(uint64_t)buf.f_bfree)/1024LL):0;
			initial_WKF_info.totalSpaceInKB = !res ? (u32)((uint64_t)((uint64_t)buf.f_bsize*(uint64_t)buf.f_blocks)/1024LL):0;		
		}
	}

	return ret;
}

extern char *getDeviceMountPath(char *str);
s32 deviceHandler_WKF_deinit(file_handle* file) {
	if(file && file->fp) {
		fclose(file->fp);
		file->fp = 0;
	}
	if(file)
		fatUnmount(getDeviceMountPath(file->name));
	return 0;
}

s32 deviceHandler_WKF_deleteFile(file_handle* file) {
	return -1;
}

s32 deviceHandler_WKF_closeFile(file_handle* file) {
    return 0;
}

bool deviceHandler_WKF_test() {
	return swissSettings.hasDVDDrive && (__wkfSpiReadId() != 0 && __wkfSpiReadId() != 0xFFFFFFFF);
}

deviceImage wkfImage = {(void *)wiikey_tpl, 0, 102, 80};
deviceImage* deviceHandler_WKF_deviceImage() { 
	wkfImage.tplSize = wiikey_tpl_size;
	return &wkfImage;
}

DEVICEHANDLER_INTERFACE __device_wkf = {
	DEVICE_ID_B,
	"Wiikey / Wasp Fusion",
	"Supported File System(s): FAT16, FAT32",
	FEAT_READ|FEAT_BOOT_GCM|FEAT_AUTOLOAD_DOL|FEAT_FAT_FUNCS|FEAT_BOOT_DEVICE|FEAT_CAN_READ_PATCHES,
	LOC_DVD_CONNECTOR,
	&initial_WKF,
	(_fn_test)&deviceHandler_WKF_test,
	(_fn_info)&deviceHandler_WKF_info,
	(_fn_init)&deviceHandler_WKF_init,
	(_fn_readDir)&deviceHandler_WKF_readDir,
	(_fn_readFile)&deviceHandler_WKF_readFile,
	(_fn_writeFile)NULL,
	(_fn_deleteFile)NULL,
	(_fn_seekFile)&deviceHandler_WKF_seekFile,
	(_fn_setupFile)&deviceHandler_WKF_setupFile,
	(_fn_closeFile)&deviceHandler_WKF_closeFile,
	(_fn_deinit)&deviceHandler_WKF_deinit,
	(_fn_deviceImage)&deviceHandler_WKF_deviceImage
};
