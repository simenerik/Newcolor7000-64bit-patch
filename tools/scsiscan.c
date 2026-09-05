/*
 * scsiscan - list everything on the SCSI buses
 *
 * Asks each SCSI port driver what devices it has found, and prints the
 * vendor/product/revision each device reports, plus whether Windows has
 * already claimed it.
 *
 * Read-only. Sends no commands to any device; only asks the port driver for
 * information it already cached at boot. Safe to run with a drum scanner
 * attached.
 *
 * Run from an Administrator command prompt.
 */

#include <windows.h>
#include <stdio.h>

#define IOCTL_SCSI_GET_INQUIRY_DATA  0x0004100C
#define IOCTL_SCSI_GET_CAPABILITIES  0x00041010

typedef struct {
    UCHAR  NumberOfLogicalUnits;
    UCHAR  InitiatorBusId;
    ULONG  InquiryDataOffset;
} SCSI_BUS_DATA_;

typedef struct {
    UCHAR          NumberOfBuses;
    SCSI_BUS_DATA_ BusData[1];
} SCSI_ADAPTER_BUS_INFO_;

typedef struct {
    UCHAR  PathId;
    UCHAR  TargetId;
    UCHAR  Lun;
    BOOLEAN DeviceClaimed;
    ULONG  InquiryDataLength;
    ULONG  NextInquiryDataOffset;
    UCHAR  InquiryData[1];
} SCSI_INQUIRY_DATA_;

typedef struct {
    ULONG Length;
    ULONG MaximumTransferLength;
    ULONG MaximumPhysicalPages;
    ULONG SupportedAsynchronousEvents;
    ULONG AlignmentMask;
    BOOLEAN TaggedQueuing;
    BOOLEAN AdapterScansDown;
    BOOLEAN AdapterUsesPio;
} IO_SCSI_CAPABILITIES_;

static const char *devtype(UCHAR t)
{
    switch (t & 0x1f) {
    case 0x00: return "disk";
    case 0x01: return "tape";
    case 0x02: return "printer";
    case 0x03: return "processor";
    case 0x04: return "write-once";
    case 0x05: return "cd/dvd";
    case 0x06: return "SCANNER";
    case 0x07: return "optical";
    case 0x08: return "changer";
    case 0x09: return "comms";
    case 0x1f: return "unknown/none";
    default:   return "other";
    }
}

static void trim(char *s)
{
    int i = (int)strlen(s);
    while (i > 0 && s[i - 1] == ' ') s[--i] = 0;
}

static int probe_port(int n)
{
    char path[64], buf[8192];
    HANDLE h;
    DWORD ret;
    SCSI_ADAPTER_BUS_INFO_ *info;
    IO_SCSI_CAPABILITIES_ caps;
    int bus, found = 0;

    sprintf(path, "\\\\.\\SCSI%d:", n);
    h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;

    printf("=== %s ===\n", path);

    if (DeviceIoControl(h, IOCTL_SCSI_GET_CAPABILITIES, NULL, 0,
                        &caps, sizeof(caps), &ret, NULL)) {
        printf("    max transfer   : %lu bytes (%lu KB)\n",
               caps.MaximumTransferLength, caps.MaximumTransferLength / 1024);
        printf("    max phys pages : %lu\n", caps.MaximumPhysicalPages);
    }

    if (!DeviceIoControl(h, IOCTL_SCSI_GET_INQUIRY_DATA, NULL, 0,
                         buf, sizeof(buf), &ret, NULL)) {
        printf("    ! GET_INQUIRY_DATA failed, error %lu\n", GetLastError());
        CloseHandle(h);
        return 1;
    }

    info = (SCSI_ADAPTER_BUS_INFO_ *)buf;
    for (bus = 0; bus < info->NumberOfBuses; bus++) {
        ULONG off = info->BusData[bus].InquiryDataOffset;
        while (off && off < sizeof(buf)) {
            SCSI_INQUIRY_DATA_ *d = (SCSI_INQUIRY_DATA_ *)(buf + off);
            if (d->InquiryDataLength >= 36) {
                char ven[9] = {0}, prod[17] = {0}, rev[5] = {0};
                memcpy(ven,  d->InquiryData + 8,  8);
                memcpy(prod, d->InquiryData + 16, 16);
                memcpy(rev,  d->InquiryData + 32, 4);
                trim(ven); trim(prod); trim(rev);
                printf("    bus %d  id %-2d  lun %-2d  %-10s  %-8s %-16s %-4s  %s\n",
                       d->PathId, d->TargetId, d->Lun,
                       devtype(d->InquiryData[0]), ven, prod, rev,
                       d->DeviceClaimed ? "** CLAIMED **" : "free");
                found++;
            }
            if (d->NextInquiryDataOffset == 0 ||
                d->NextInquiryDataOffset == off) break;
            off = d->NextInquiryDataOffset;
        }
    }
    if (!found) printf("    (no devices reported on this port)\n");
    CloseHandle(h);
    return 1;
}

int main(void)
{
    int i, ports = 0;

    printf("scsiscan - SCSI bus inventory\n");
    printf("%s build\n\n", sizeof(void *) == 8 ? "64-bit" : "32-bit");

    for (i = 0; i < 32; i++) ports += probe_port(i);

    if (!ports) {
        printf("No SCSI ports could be opened.\n\n");
        printf("  * Are you running as Administrator?\n");
        printf("  * Is the SCSI card installed and showing cleanly in Device Manager?\n");
        return 1;
    }

    printf("\nA line showing type SCANNER with a Heidelberg-ish vendor string is\n");
    printf("what you are looking for. \"free\" is good - it means nothing else has\n");
    printf("taken the device and pass-through can reach it.\n");
    return 0;
}
