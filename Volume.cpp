/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/param.h>

#include <linux/kdev_t.h>

#include <cutils/properties.h>

#include <diskconfig/diskconfig.h>

#include <private/android_filesystem_config.h>

#define LOG_TAG "Vold"

#include <cutils/fs.h>
#include <cutils/log.h>

#include <string>

#include "Ntfs.h"
#include "Volume.h"
#include "VolumeManager.h"
#include "ResponseCode.h"
#include "Fat.h"
#include "Process.h"
#include "cryptfs.h"

#ifdef SUPPORTED_MULTI_USB_PARTITIONS 
#include "blkid/blkid.h"  
#include "unicode/ucnv.h"
#define MAX_DEVICE_NODES 32
#endif

extern "C" void dos_partition_dec(void const *pp, struct dos_partition *d);
extern "C" void dos_partition_enc(void *pp, struct dos_partition *d);


/*
 * Media directory - stuff that only media_rw user can see
 */
const char *Volume::MEDIA_DIR           = "/mnt";//"/mnt/media_rw

/*
 * Fuse directory - location where fuse wrapped filesystems go
 */
const char *Volume::FUSE_DIR           = "/storage";

/*
 * Path to external storage where *only* root can access ASEC image files
 */
const char *Volume::SEC_ASECDIR_EXT   = "/mnt/secure/asec";

/*
 * Path to internal storage where *only* root can access ASEC image files
 */
const char *Volume::SEC_ASECDIR_INT   = "/data/app-asec";

/*
 * Path to where secure containers are mounted
 */
const char *Volume::ASECDIR           = "/mnt/asec";

/*
 * Path to where OBBs are mounted
 */
const char *Volume::LOOPDIR           = "/mnt/obb";

const char *Volume::BLKID_PATH = "/system/bin/blkid";

static const char *stateToStr(int state) {
    if (state == Volume::State_Init)
        return "Initializing";
    else if (state == Volume::State_NoMedia)
        return "No-Media";
    else if (state == Volume::State_Idle)
        return "Idle-Unmounted";
    else if (state == Volume::State_Pending)
        return "Pending";
    else if (state == Volume::State_Mounted)
        return "Mounted";
    else if (state == Volume::State_Unmounting)
        return "Unmounting";
    else if (state == Volume::State_Checking)
        return "Checking";
    else if (state == Volume::State_Formatting)
        return "Formatting";
    else if (state == Volume::State_Shared)
        return "Shared-Unmounted";
    else if (state == Volume::State_SharedMnt)
        return "Shared-Mounted";
    else
        return "Unknown-Error";
}

Volume::Volume(VolumeManager *vm, const fstab_rec* rec, int flags) {
    mVm = vm;
    mDebug = false;
    mLabel = strdup(rec->label);
    mUuid = NULL;
    mUserLabel = NULL;
    mState = Volume::State_Init;
    mFlags = flags;
    mCurrentlyMountedKdev = -1;
    mPartIdx = rec->partnum;
    mRetryMount = false;
	mSkipAsec =false;
#ifdef SUPPORTED_MULTI_USB_PARTITIONS
    mLetters = 0;
#endif
}

Volume::~Volume() {
    free(mLabel);
    free(mUuid);
    free(mUserLabel);
}

void Volume::setDebug(bool enable) {
    mDebug = enable;
}

dev_t Volume::getDiskDevice() {
    return MKDEV(0, 0);
};

dev_t Volume::getShareDevice() {
    return getDiskDevice();
}

void Volume::handleVolumeShared() {
}

void Volume::handleVolumeUnshared() {
}

int Volume::handleBlockEvent(NetlinkEvent *evt) {
    errno = ENOSYS;
    return -1;
}

void Volume::setUuid(const char* uuid) {
    char msg[256];

    if (mUuid) {
        free(mUuid);
    }

    if (uuid) {
        mUuid = strdup(uuid);
        snprintf(msg, sizeof(msg), "%s %s \"%s\"", getLabel(),
                getFuseMountpoint(), mUuid);
    } else {
        mUuid = NULL;
        snprintf(msg, sizeof(msg), "%s %s", getLabel(), getFuseMountpoint());
    }

    mVm->getBroadcaster()->sendBroadcast(ResponseCode::VolumeUuidChange, msg,
            false);
}

void Volume::setUserLabel(const char* userLabel) {
    char msg[256];

    if (mUserLabel) {
        free(mUserLabel);
    }

    if (userLabel) {
        mUserLabel = strdup(userLabel);
        snprintf(msg, sizeof(msg), "%s %s \"%s\"", getLabel(),
                getFuseMountpoint(), mUserLabel);
    } else {
        mUserLabel = NULL;
        snprintf(msg, sizeof(msg), "%s %s", getLabel(), getFuseMountpoint());
    }

    mVm->getBroadcaster()->sendBroadcast(ResponseCode::VolumeUserLabelChange,
            msg, false);
}

//to inform SDMMC-driver for umounting sdcard. noted by xbw@2011-06-07
void Volume::notifyStateKernel(int number)
{
    if(!strncmp(getLabel(),"external_sd", strlen("external_sd")))
    {
        FILE *fp = fopen("/sys/sd-sdio/rescan","w");
        if(fp){
            char kstate[64] = "sd-";

            if(number == 0) {
                strcat(kstate,"Ready");//strcat(kstate,stateToStr(Volume::State_Ready));
            } else {
                strcat(kstate,stateToStr(mState));
            }
            fputs(kstate,fp);
            SLOGI("Call notifyStateKernel No.%d in the file of Volume.cpp", number);
            fclose(fp);
        } else {
            SLOGI("Error(call No.%d) opening /sys/sd-sdio/rescan in the file of VOlume.cpp", number);
        }
    }
}


void Volume::setState(int state) {
    char msg[255];
    int oldState = mState;

    if (oldState == state) {
        SLOGW("Duplicate state (%d)\n", state);
        return;
    }

    if ((oldState == Volume::State_Pending) && (state != Volume::State_Idle)) {
        mRetryMount = false;
    }

    mState = state;
	notifyStateKernel(1);//add by xbw

    SLOGD("Volume %s state changing %d (%s) -> %d (%s)", mLabel,
         oldState, stateToStr(oldState), mState, stateToStr(mState));
    snprintf(msg, sizeof(msg),
             "Volume %s %s state changed from %d (%s) to %d (%s)", getLabel(),
             getFuseMountpoint(), oldState, stateToStr(oldState), mState,
             stateToStr(mState));

    mVm->getBroadcaster()->sendBroadcast(ResponseCode::VolumeStateChange,
                                         msg, false);
}

int Volume::createDeviceNode(const char *path, int major, int minor) {
    mode_t mode = 0660 | S_IFBLK;
    dev_t dev = (major << 8) | minor;
    if (mknod(path, mode, dev) < 0) {
        if (errno != EEXIST) {
            return -1;
        }
    }
    return 0;
}

int Volume::formatVol(bool wipe) {

    if (getState() == Volume::State_NoMedia) {
        errno = ENODEV;
        return -1;
    } else if (getState() != Volume::State_Idle) {
        errno = EBUSY;
        return -1;
    }

    if (isMountpointMounted(getMountpoint())) {
        SLOGW("Volume is idle but appears to be mounted - fixing");
        setState(Volume::State_Mounted);
        // mCurrentlyMountedKdev = XXX
        errno = EBUSY;
        return -1;
    }

    bool formatEntireDevice = (mPartIdx == -1);
    char devicePath[255];
    char label[PROPERTY_VALUE_MAX] = "";
    dev_t diskNode = getDiskDevice();
    dev_t partNode =
        MKDEV(MAJOR(diskNode),
              MINOR(diskNode) + (formatEntireDevice ? 1 : mPartIdx));

    setState(Volume::State_Formatting);
    int ret = -1;
	
    if (!strcmp(getLabel(),"internal_sd")) {
    	property_get("UserVolumeLabel", label, "");
        formatEntireDevice = false;
    }
    // Only initialize the MBR if we are formatting the entire device
    if (formatEntireDevice) {
        sprintf(devicePath, "/dev/block/vold/%d:%d",
                MAJOR(diskNode), MINOR(diskNode));

        if (initializeMbr(devicePath)) {
            SLOGE("Failed to initialize MBR (%s)", strerror(errno));
            goto err;
        }
    }

    if (!strcmp(getLabel(),"internal_sd") && MAJOR(diskNode) != 179) {
        sprintf(devicePath, "/dev/block/vold/%d:%d",
                MAJOR(diskNode), MINOR(diskNode));
    } else {
        sprintf(devicePath, "/dev/block/vold/%d:%d",
                MAJOR(partNode), MINOR(partNode));
    }

    if (mDebug) {
        SLOGI("Formatting volume %s (%s)", getLabel(), devicePath);
    }

    if (Fat::format(devicePath, 0, wipe, label)) {
        SLOGE("Failed to format (%s)", strerror(errno));
        goto err;
    }
    if (!strcmp(getLabel(),"internal_sd")) {
        system("sync");
    }

    ret = 0;

err:
    setState(Volume::State_Idle);
    return ret;
}

bool Volume::isMountpointMounted(const char *path) {
    char device[256];
    char mount_path[256];
    char rest[256];
    FILE *fp;
    char line[1024];

    if (!path)
    {
        SLOGE("isMountpointMounted path is NULL !");
        return false;
    }

    if (!(fp = fopen("/proc/mounts", "r"))) {
        SLOGE("Error opening /proc/mounts (%s)", strerror(errno));
        return false;
    }

    while(fgets(line, sizeof(line), fp)) {
        line[strlen(line)-1] = '\0';
        sscanf(line, "%255s %255s %255s\n", device, mount_path, rest);
        if (!strcmp(mount_path, path)) {
            fclose(fp);
            return true;
        }
    }

    fclose(fp);
    return false;
}

int Volume::mountVol() {
    dev_t deviceNodes[4];
    int n, i, rc = 0;
    char errmsg[255];

    int flags = getFlags();
    bool providesAsec = (flags & VOL_PROVIDES_ASEC) != 0;

    // TODO: handle "bind" style mounts, for emulated storage

    char decrypt_state[PROPERTY_VALUE_MAX];
    char crypto_state[PROPERTY_VALUE_MAX];
    char encrypt_progress[PROPERTY_VALUE_MAX];
    char has_ums[PROPERTY_VALUE_MAX];

    property_get("vold.decrypt", decrypt_state, "");
    property_get("vold.encrypt_progress", encrypt_progress, "");
    property_get("ro.factory.hasUMS",has_ums, "false");

    char getSupNtfs[PROPERTY_VALUE_MAX];
    property_get("ro.factory.storage_suppntfs",getSupNtfs, "true");
	bool isSupNtfs = !strcmp(getSupNtfs,"true");
    /* Don't try to mount the volumes if we have not yet entered the disk password
     * or are in the process of encrypting.
     */
    if ((getState() == Volume::State_NoMedia) ||
        ((!strcmp(decrypt_state, "1") || encrypt_progress[0]) && providesAsec)) {
        snprintf(errmsg, sizeof(errmsg),
                 "Volume %s %s mount failed - no media",
                 getLabel(), getFuseMountpoint());
        mVm->getBroadcaster()->sendBroadcast(
                                         ResponseCode::VolumeMountFailedNoMedia,
                                         errmsg, false);
        errno = ENODEV;
        return -1;
    } else if (getState() != Volume::State_Idle) {
        errno = EBUSY;
        if (getState() == Volume::State_Pending) {
            mRetryMount = true;
        }
        return -1;
    }
#ifdef SUPPORTED_MULTI_USB_PARTITIONS
    if(strcmp(getLabel(),USB_DISK_LABEL))
#endif
    { 
        if (isMountpointMounted(getMountpoint())) {
            SLOGW("Volume is idle but appears to be mounted - fixing");
            setState(Volume::State_Mounted);
            // mCurrentlyMountedKdev = XXX
            return 0;
        }
    }
#ifdef SUPPORTED_MULTI_USB_PARTITIONS
    else
    {
        char devicePath[255];                                                                                                                
        n = getDeviceNodes((dev_t *) &deviceNodes, MAX_DEVICE_NODES);
        for (int i=0;i<n;i++)
        {
            int major = MAJOR(deviceNodes[i]);
            int minor = MINOR(deviceNodes[i]);
            sprintf(devicePath, "/dev/block/vold/%d:%d", major,minor);
            if (!isMountpointMounted(getUdiskMountpoint(devicePath,major,minor,NULL))){
                goto UDISKNOMOUNTED;
            }
        }
        SLOGW("UDisk Volume is idle but appears to be mounted - fixing");
        setState(Volume::State_Mounted);
        return 0;
    }
UDISKNOMOUNTED:
#endif
    n = getDeviceNodes((dev_t *) &deviceNodes, 4);
    if (!n) {
        SLOGE("Failed to get device nodes (%s)\n", strerror(errno));
        return -1;
    }

    /* If we're running encrypted, and the volume is marked as encryptable and nonremovable,
     * and also marked as providing Asec storage, then we need to decrypt
     * that partition, and update the volume object to point to it's new decrypted
     * block device
     */
    property_get("ro.crypto.state", crypto_state, "");
    if (providesAsec &&
        ((flags & (VOL_NONREMOVABLE | VOL_ENCRYPTABLE))==(VOL_NONREMOVABLE | VOL_ENCRYPTABLE)) &&
        !strcmp(crypto_state, "encrypted") && !isDecrypted()) {
       char new_sys_path[MAXPATHLEN];
       char nodepath[256];
       int new_major, new_minor;

       if (n != 1) {
           /* We only expect one device node returned when mounting encryptable volumes */
           SLOGE("Too many device nodes returned when mounting %d\n", getMountpoint());
           return -1;
       }

       if (cryptfs_setup_volume(getLabel(), MAJOR(deviceNodes[0]), MINOR(deviceNodes[0]),
                                new_sys_path, sizeof(new_sys_path),
                                &new_major, &new_minor)) {
           SLOGE("Cannot setup encryption mapping for %d\n", getMountpoint());
           return -1;
       }
       /* We now have the new sysfs path for the decrypted block device, and the
        * majore and minor numbers for it.  So, create the device, update the
        * path to the new sysfs path, and continue.
        */
        snprintf(nodepath,
                 sizeof(nodepath), "/dev/block/vold/%d:%d",
                 new_major, new_minor);
        if (createDeviceNode(nodepath, new_major, new_minor)) {
            SLOGE("Error making device node '%s' (%s)", nodepath,
                                                       strerror(errno));
        }

        // Todo: Either create sys filename from nodepath, or pass in bogus path so
        //       vold ignores state changes on this internal device.
        updateDeviceInfo(nodepath, new_major, new_minor);

        /* Get the device nodes again, because they just changed */
        n = getDeviceNodes((dev_t *) &deviceNodes, 4);
        if (!n) {
            SLOGE("Failed to get device nodes (%s)\n", strerror(errno));
            return -1;
        }
    }

    for (i = 0; i < n; i++) {
        char devicePath[255];

        sprintf(devicePath, "/dev/block/vold/%d:%d", MAJOR(deviceNodes[i]),
                MINOR(deviceNodes[i]));

        SLOGI("%s being considered for volume %s,%d\n ", devicePath, getLabel(),isSupNtfs);

        errno = 0;
        setState(Volume::State_Checking);

        if (Fat::check(devicePath) && !isSupNtfs) {
            if (errno == ENODATA) {
                SLOGW("%s does not contain a FAT filesystem\n", devicePath);
                continue;
            }
            errno = EIO;
            /* Badness - abort the mount */
            SLOGE("%s failed FS checks (%s)", devicePath, strerror(errno));
            setState(Volume::State_Idle);
            return -1;
        }

        errno = 0;
        int gid;

        char mount_point[255]={0};
#ifdef SUPPORTED_MULTI_USB_PARTITIONS
        char letter = 'x'; 
        if (!strcmp(getLabel(),USB_DISK_LABEL)){
            int major = MAJOR(deviceNodes[i]);
            int minor = MINOR(deviceNodes[i]);
            letter = getNextLetter();
            const char *mountpoint = getUdiskMountpoint(devicePath,major,minor,letter);
            strcpy(mount_point,mountpoint);

            if (mkdir(mount_point,0000))
                SLOGD("mountVol,mountpoint : %s",mount_point);

            setState(Volume::State_Checking);
        }
        else
#endif
        {
            const char *mountpoint = getMountpoint();
            strcpy(mount_point,mountpoint);
        }
	    if(!strcmp("true",has_ums))//has UMS function ,set group to AID_SDCARD_RW
	    {
        	if (Fat::doMount(devicePath, mount_point, false, false, false,
        	        AID_SYSTEM,AID_SDCARD_RW, 0002, true)) {
        		SLOGE("%s failed to mount via VFAT (%s)\n", devicePath, strerror(errno));
        			if(providesAsec)
        			{
						mSkipAsec = true;
						SLOGE("---------set mSkipAsec to disable app2sd because mount Vfat fail for %s, mountpoint =%s",getLabel(),getMountpoint());
					}
                	if(Ntfs::doMount(devicePath, mount_point, false, 1000)){ 
               			SLOGE("%s failed to mount via VNTFS (%s)\n", devicePath, strerror(errno));
#ifdef SUPPORTED_MULTI_USB_PARTITIONS
                        if (!strcmp(getLabel(),USB_DISK_LABEL)){
                            setState(Volume::State_Idle);
                            rmdir(mount_point);
                            releaseLetter(letter);
                        }
#endif
                		continue;
               		}
        	}
			else//mount flash as fat succeed
			{
				mSkipAsec = false;
				SLOGE("---------set mSkipAsec to enable app2sd because mount Vfat succeed for %s, mountpoint =%s",getLabel(),getMountpoint());
			}
	    }
	    else //do not has ums,set group to AID_MEDIA_RW
	    {
        	if (Fat::doMount(devicePath, mount_point, false, false, false,
               		AID_SYSTEM,AID_MEDIA_RW, 0002, true)) {
            		SLOGE("%s failed to mount via VFAT (%s)\n", devicePath, strerror(errno));
            
			    if(Ntfs::doMount(devicePath, mount_point, false, 1000)){
               			SLOGE("%s failed to mount via VNTFS (%s)\n", devicePath, strerror(errno));
#ifdef SUPPORTED_MULTI_USB_PARTITIONS
                        if (!strcmp(getLabel(),USB_DISK_LABEL)){
                            setState(Volume::State_Idle);
                            rmdir(mount_point);
                            releaseLetter(letter);
                        }
#endif
		       		continue;
		     	}
        	}
	    }   

#ifdef SUPPORTED_MULTI_USB_PARTITIONS
        if (!strcmp(getLabel(),USB_DISK_LABEL)){
            int major = MAJOR(deviceNodes[i]);
            int minor = MINOR(deviceNodes[i]);
            VolumePartition* pt = new VolumePartition();
            pt->major = major;
            pt->minor = minor;
            pt->letter = letter;
            strcpy(pt->mountpoint,mount_point);
            mPartitions.push_back(pt);
        }
#endif
        extractMetadata(devicePath);

        if (providesAsec&&!mSkipAsec&& mountAsecExternal() != 0) {
            SLOGE("Failed to mount secure area (%s)", strerror(errno));
            umount(mount_point);
            setState(Volume::State_Idle);
            return -1;
        }

        char service[64];
        snprintf(service, 64, "fuse_%s", getLabel());
        property_set("ctl.start", service);

        setState(Volume::State_Mounted);
        mCurrentlyMountedKdev = deviceNodes[i];
#ifdef SUPPORTED_MULTI_USB_PARTITIONS
        if (!strcmp(getLabel(),USB_DISK_LABEL))
            continue;
#endif
        return 0;
    }

    SLOGE("Volume %s found no suitable devices for mounting :(\n", getLabel());
#ifdef SUPPORTED_MULTI_USB_PARTITIONS
    if (strcmp(getLabel(),USB_DISK_LABEL))
    {
        SLOGE("Volume %s found no suitable devices for mounting :(\n", getLabel());
        setState(Volume::State_Idle);
    }   
    else
    {
        SLOGD("Volume usb_storage mounted .");
        setState(Volume::State_Mounted);
        return 0;
    }
#else
    setState(Volume::State_Idle);
#endif
    return -1;
}

int Volume::mountAsecExternal() {
    char legacy_path[PATH_MAX];
    char secure_path[PATH_MAX];
    char has_ums[PROPERTY_VALUE_MAX];

    property_get("ro.factory.hasUMS",has_ums, "false");


    snprintf(legacy_path, PATH_MAX, "%s/android_secure", getMountpoint());
    snprintf(secure_path, PATH_MAX, "%s/.android_secure", getMountpoint());

    // Recover legacy secure path
    if (!access(legacy_path, R_OK | X_OK) && access(secure_path, R_OK | X_OK)) {
        if (rename(legacy_path, secure_path)) {
            SLOGE("Failed to rename legacy asec dir (%s)", strerror(errno));
        }
    }

    if(!strcmp("true",has_ums))//has UMS function ,set group to AID_SDCARD_RW
    {
    	if (fs_prepare_dir(secure_path, 0770, AID_SYSTEM,AID_SDCARD_RW) != 0) {
        	return -1;
    	}
    }
    else
    {
    	if (fs_prepare_dir(secure_path, 0770, AID_SYSTEM,AID_MEDIA_RW) != 0) {
        	return -1;
    	}
    }

    if (mount(secure_path, SEC_ASECDIR_EXT, "", MS_BIND, NULL)) {
        SLOGE("Failed to bind mount points %s -> %s (%s)", secure_path,
                SEC_ASECDIR_EXT, strerror(errno));
        return -1;
    }
	property_set("sys.vold.hasAsec","true"); 
    return 0;
}

int Volume::doUnmount(const char *path, bool force) {
    int retries = 150;

    if (mDebug) {
        SLOGD("Unmounting {%s}, force = %d", path, force);
    }

    while (retries--) {
        if (!umount(path) || errno == EINVAL || errno == ENOENT) {
            SLOGI("%s sucessfully unmounted", path);
			notifyStateKernel(2);
            return 0;
        }

        int action = 0;
		notifyStateKernel(3);
        if (force) {
            if (retries <= 120) {
                action = 2; // SIGKILL
            } else if (retries <= 130) {
                action = 1; // SIGHUP
            }
        }
		if(retries%10==0)
        	SLOGW("Failed to unmount %s (%s, retries %d, action %d)",
                path, strerror(errno), retries, action);

        Process::killProcessesWithOpenFiles(path, action);
        usleep(1000*30);
    }
    errno = EBUSY;
    SLOGE("Giving up on unmount %s (%s)", path, strerror(errno));
    return -1;
}

int Volume::unmountVol(bool force, bool revert) {
    int i, rc;

    int flags = getFlags();
    bool providesAsec = ((flags & VOL_PROVIDES_ASEC) != 0)&&(!mSkipAsec);
    revert = mPartIdx == -1 ? false : revert;

    if (getState() != Volume::State_Mounted) {
        SLOGE("Volume %s unmount request when not mounted", getLabel());
        errno = EINVAL;
        return UNMOUNT_NOT_MOUNTED_ERR;
    }

    setState(Volume::State_Unmounting);
    usleep(1000 * 1000); // Give the framework some time to react

    char service[64];
    snprintf(service, 64, "fuse_%s", getLabel());
    property_set("ctl.stop", service);
    /* Give it a chance to stop.  I wish we had a synchronous way to determine this... */
    sleep(1);

    // TODO: determine failure mode if FUSE times out

    if (providesAsec) {
		if(doUnmount(Volume::SEC_ASECDIR_EXT, force) != 0)
		{
        	SLOGE("Failed to unmount secure area on %s (%s)", getMountpoint(), strerror(errno));
        	goto out_mounted;
		}
		else
		{
			property_set("sys.vold.hasAsec","false"); 
			SLOGE("Succeed to umount secure area on %s",getMountpoint());
		}
    }

    /* Now that the fuse daemon is dead, unmount it */
    if (doUnmount(getFuseMountpoint(), force) != 0) {
        SLOGE("Failed to unmount %s (%s)", getFuseMountpoint(), strerror(errno));
        //goto fail_remount_secure;
    }

    /* Unmount the real sd card */
    if (doUnmount(getMountpoint(), force) != 0) {
        SLOGE("Failed to unmount %s (%s)", getMountpoint(), strerror(errno));
        goto fail_remount_secure;
    }

    SLOGI("%s unmounted successfully", getMountpoint());

    /* If this is an encrypted volume, and we've been asked to undo
     * the crypto mapping, then revert the dm-crypt mapping, and revert
     * the device info to the original values.
     */
    if (revert && isDecrypted()) {
        cryptfs_revert_volume(getLabel());
        revertDeviceInfo();
        SLOGI("Encrypted volume %s reverted successfully", getMountpoint());
    }

    setUuid(NULL);
    setUserLabel(NULL);
    setState(Volume::State_Idle);
    mCurrentlyMountedKdev = -1;
    return 0;

fail_remount_secure:
    if (providesAsec && mountAsecExternal() != 0) {
        SLOGE("Failed to remount secure area (%s)", strerror(errno));
        goto out_nomedia;
    }

out_mounted:
    setState(Volume::State_Mounted);
    return -1;

out_nomedia:
    setState(Volume::State_NoMedia);
    return -1;
}

#ifdef SUPPORTED_MULTI_USB_PARTITIONS
int Volume::unmountPartition(int major, int minor){
    setState(Volume::State_Unmounting);
    Partitions::iterator ir;
    for (ir = mPartitions.begin(); ir != mPartitions.end(); ++ir)
    {
       if (((VolumePartition*)*ir)->major == major && ((VolumePartition*)*ir)->minor == minor)
       {
           SLOGD("Unmounting partition : %d ",((VolumePartition*)*ir)->mountpoint);
           if (doUnmount(((VolumePartition*)*ir)->mountpoint, true)){
           SLOGE("Failed to unmount  (%s)",  strerror(errno));
            }else {
           rmdir(((VolumePartition*)*ir)->mountpoint);
           SLOGD("Success to unmount %s",((VolumePartition*)*ir)->mountpoint);
           releaseLetter(((VolumePartition*)*ir)->letter);
                delete(*ir);
       mPartitions.erase(ir);
       }
       break;
   }
    }  
    return 0;
}
#endif

int Volume::initializeMbr(const char *deviceNode) {
    struct disk_info dinfo;

    memset(&dinfo, 0, sizeof(dinfo));

    if (!(dinfo.part_lst = (struct part_info *) malloc(MAX_NUM_PARTS * sizeof(struct part_info)))) {
        SLOGE("Failed to malloc prt_lst");
        return -1;
    }

    memset(dinfo.part_lst, 0, MAX_NUM_PARTS * sizeof(struct part_info));
    dinfo.device = strdup(deviceNode);
    dinfo.scheme = PART_SCHEME_MBR;
    dinfo.sect_size = 512;
    dinfo.skip_lba = 2048;
    dinfo.num_lba = 0;
    dinfo.num_parts = 1;

    struct part_info *pinfo = &dinfo.part_lst[0];

    pinfo->name = strdup("android_sdcard");
    pinfo->flags |= PART_ACTIVE_FLAG;
    pinfo->type = PC_PART_TYPE_FAT32;
    pinfo->len_kb = -1;

    int rc = apply_disk_config(&dinfo, 0);

    if (rc) {
        SLOGE("Failed to apply disk configuration (%d)", rc);
        goto out;
    }

 out:
    free(pinfo->name);
    free(dinfo.device);
    free(dinfo.part_lst);

    return rc;
}

/*
 * Use blkid to extract UUID and label from device, since it handles many
 * obscure edge cases around partition types and formats. Always broadcasts
 * updated metadata values.
 */
int Volume::extractMetadata(const char* devicePath) {
    int res = 0;

    std::string cmd;
    cmd = BLKID_PATH;
    cmd += " -c /data/data/blkid ";
    cmd += devicePath;

    FILE* fp = popen(cmd.c_str(), "r");
    if (!fp) {
        ALOGE("Failed to run %s: %s", cmd.c_str(), strerror(errno));
        res = -1;
        goto done;
    }

    char line[1024];
    char value[128];
    if (fgets(line, sizeof(line), fp) != NULL) {
        ALOGD("blkid identified as %s", line);

        char* start = strstr(line, "UUID=");
        if (start != NULL && sscanf(start + 5, "\"%127[^\"]\"", value) == 1) {
            setUuid(value);
        } else {
            setUuid(NULL);
        }

        start = strstr(line, "LABEL=");
        if (start != NULL && sscanf(start + 6, "\"%127[^\"]\"", value) == 1) {
            setUserLabel(value);
        } else {
            setUserLabel(NULL);
        }
    } else {
        ALOGW("blkid failed to identify %s", devicePath);
        //res = -1;
        int fd = open("/data/data/blkid", O_RDONLY);
		if(fd<0)
		{
			ALOGD("---extractMetadata open cache fail---");
			res = -1;
			remove("/data/data/blkid");
			goto done;
		}
		int nbytes = read(fd, line, sizeof(line) - 1);
		if (nbytes <= 0) 
		{
			ALOGD("---extractMetadata read cache fail---");
			res = -1;
			close(fd);
			remove("/data/data/blkid");
			goto done;
		}
        char* start = strstr(line, devicePath);
		start=strstr(line, "UUID=");
        if (start != NULL && sscanf(start + 5, "\"%127[^\"]\"", value) == 1) {
            setUuid(value);
			ALOGD("---get uuid =%s",value);
        } else {
            setUuid(NULL);
        }

        start = strstr(line, "LABEL=");
        if (start != NULL && sscanf(start + 6, "\"%127[^\"]\"", value) == 1) {
            setUserLabel(value);
			ALOGD("---get label =%s",value);
        } else {
            setUserLabel(NULL);
        }
		close(fd);
		remove("/data/data/blkid");
    }

    pclose(fp);

done:
    if (res == -1) {
        setUuid(NULL);
        setUserLabel(NULL);
    }
    return res;
}

#ifdef SUPPORTED_MULTI_USB_PARTITIONS
/*
* getVolumeLabel
*  use blkid to get volume label
*/
void Volume::getVolumeLabel(const char* devicePath,char* vLabel,char letter){
   char *pfstype;
   char *pfslabel;
   blkid_cache cache = NULL;
   
   blkid_get_cache(&cache, "/dev/null");
   pfstype = blkid_get_tag_value(cache, "TYPE",devicePath);
   pfslabel = blkid_get_tag_value(cache, "LABEL",devicePath);
   blkid_put_cache(cache);

   
   SLOGD(">>> getVolumeLabel .letter:%c",letter);

   /* if you want to change mountpoint format ,
    *  change codes below.
    * WARNING:
    *  1. BE SURE TO UNIQUENESS OF MOUNTPOINT NAME !!!
   */
   //SLOGD("getVolumeLabel : type : %s , label : %s",pfstype,pfslabel);
   if(pfslabel) // success to get volume label
   {
       /* 
       //JUST FOR DEBUG UNKONWN CODE VOLUM LABEL
       char* p = pfslabel;
       while(1){
           SLOGD("pfslabel : %02x",*p);
           if (*(++p) == 0)
               break;
       }*/

       char utf8Label[255] = {0};
       if (!strcmp(pfstype,"vfat"))
       {
           gb2312_to_utf8(utf8Label, 255, pfslabel, strlen(pfslabel));
       }
       else
       {
           strcpy(utf8Label,pfslabel);
       }

       //sprintf(vLabel,"%d_%d(%s)",major,minor,utf8Label);
       sprintf(vLabel,"%c(%s)",letter,utf8Label);

       free(pfstype);
       free(pfslabel);
       pfstype =NULL;
       pfslabel = NULL;
   }
   else // unknow volume label
   {
       //sprintf(vLabel,"%d_%d(udisk)",major,minor);
       sprintf(vLabel,"%c(udisk)",letter);
   }

   SLOGD("getVolumeLabel : devicePath : %s , type : %s , label : %s ",devicePath,pfstype,vLabel);
   
}

size_t Volume::gb2312_to_utf8(char* pOut,size_t pOutLen, char* pIn, size_t pInlen)
{
   UErrorCode ErrorCode = U_ZERO_ERROR;
   size_t ret = ucnv_convert("utf-8","gbk",pOut,pOutLen,pIn,pInlen,&ErrorCode);
   return 0;
}

char Volume::getNextLetter()
{
    /*
     * A = 65 , C = 67 , Z = 90
     * mLetters first bit = A
    */
    int i = 1,n = 0;
    for (n = 0; n < 26; n++)
    {
        SLOGW("i:%d n:%d mLetters :%d",i,n,mLetters);
        if (i & mLetters)
        {
            i = 1 << (n + 1);
        }
        else
        {
            SLOGW("n : %d",n);
            mLetters |= 1 << n;
            return (char)(n + 65);
        }
            
    }
    return 0;
}

void Volume::releaseLetter(char letter)
{
    mLetters &= (~(1 << (int32_t)letter-65));
}
#endif 
