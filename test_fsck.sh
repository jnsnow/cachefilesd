#!/bin/bash

rootdir=/var/cache/xfscache
numtests=7
xattrstr="user.CacheFiles.cache"

# change_slot_to <file> <len> <slot>
function change_slot_to {
    file="$1"
    len="$2"
    slot="$3"

    getfattr --absolute-names --dump -e hex "$file" > tmp.xattr
    sed -i -r "s|0x[[:xdigit:]]{2}|0x${slot}|" tmp.xattr
    setfattr --restore=tmp.xattr
}

num=$(find ${rootdir}/cache/ -type f -print | wc -l)
if (( num < numtests )); then
    echo "Not enough files in the cache for a full test."
    exit
fi

# Read in a couple of files to toy around with
IFS=$'
'
files=($(find ${rootdir}/cache -type f -print | head -${numtests}));
unset IFS


### -- Mutilate the Files -- ###


# Missing Xattrs
echo "1: Removing xattrs on ${files[0]}"
setfattr --remove=${xattrstr} ${files[0]}

# Wrong Xattr
getfattr --absolute-names --dump -e hex ${files[1]} > tmp.xattr
slot=$(grep -oE '0x[[:xdigit:]]{2}' tmp.xattr)
echo "2: Corrupting xattrs on ${files[1]} by increasing his slot ${slot} by one."
((slot++));
newslot=$(printf "0x%02x" $slot);
sed -i -r "s|0x[[:xdigit:]]{2}|${newslot}|" tmp.xattr
setfattr --restore=tmp.xattr

# Out of Bounds Xattr (Estimated -- 255 > ~157)
echo "3: Corrupting ${files[2]} by changing slot to 255"
change_slot_to "${files[2]}" 2 "FF"

# PINNED xattr
echo "4: Corrupting ${files[3]} by changing slot to PINNED"
change_slot_to "${files[3]}" 8 "FEFFFFFF"

# NO_CULL_SLOT xattr
echo "5: Corrupting ${files[4]} by changing slot to NO_CULL_SLOT (-1)"
change_slot_to "${files[4]}" 8 "FFFFFFFF"

# Wrong Name -- file handle verification will fail.
echo "6: Corrupting ${files[5]} by renaming it to 'incorrectly_named'"
newname6="$(dirname ${files[5]})/incorrectly_named"
mv "${files[5]}" "${newname6}"
if [ ! -e "${newname6}" ]; then
    echo "${newname6}"
    echo "file name prediction failure, #6"
fi

# Wrong Dir -- file handle verification will identify an incorrect parent.
newname7="$(dirname ${files[5]})/$(basename ${files[6]})"
echo "moving ${files[6]} to be ${newname7}, in a wrong parent dir."
mv "${files[6]}" "${newname7}"
if [ ! -e "${newname7}" ]; then
    echo "${newname7}"
    echo "file name prediction failure, #7"
fi

exit 0
sudo ./cachefilesd -dd -s -n -c -F

## Check Results ##

# Missing xattr file should be deleted, even thoush it still has an index slot.
# The index scan will see it as a possibly stale or re-used file handle and delete the slot.
# The file scan will see the orphaned file and delete it.
# Anticipated output: (examples)
# Suspected stale filehandle: slot #5 points to a file with missing xattr property. Deleting slot.
# [EE0g00sgw1200200000000QL00000000000000000-G7c400000wd3xNK] doesn't have the correct xattrs.
if [ -e "${files[0]}" ]; then
    echo ""
    echo "1: Missing Xattr Test FAILED (file was not deleted.)"
fi

# Wrong xattr file should have its slot corrected during the index scanning phase.
# Anticipated output: (example)
# Slot #15 points to a file which points back to slot #16. Correcting xattrs.
if [ ! -e "${files[1]}" ]; then
    echo ""
    echo "2: Wrong Xattr Test FAILED (file went missing.)"
fi

# Out of Bounds xattr test should have its slot corrected during the index scan.
# Anticipated output: (example)
# Slot #18 points to a file which points back to slot #255. Correcting xattrs.
if [ ! -e "${files[2]}" ]; then
    echo ""
    echo "3: Out-of-Bounds Xattr Test FAILED (file went missing.)"
fi

# A file with a PINNED attribute is left alone, and the index is deleted during the index scan.
# Anticipated output: (example)
# Slot #19 is presumably a duplicate. Removing slot.
if [ ! -e "${files[3]}" ]; then
    echo ""
    echo "4: PINNED Xattr Test FAILED (file went missing)"
fi

# A file with a NO_CULL_SLOT attribute should have its xattr corrected during the index scan.
# Anticipated output: (example)
# Slot #20 points to a file which points back to slot #4294967295. Correcting xattrs.
if [ ! -e "${files[4]}" ]; then
    echo ""
    echo "5: NO_CULL_SLOT Xattr Test FAILED (file went missing)"
fi

# A file with an incorrect filename won't be detected by the index scan.
# During the file scan, however, it will be detected as having a bad name,
# and be deleted.
# Anticipated output: (example)
# [incorrectly_named] has a bad name, or bad name/type combo. Deleting.
if [ -e "${newname6}" ]; then
    echo ""
    echo "6: Wrong Name Test FAILED (file was not deleted)"
fi

# During the file-spidering, the file will be detected as
# being in the wrong directory and deleted.
# Anticipated output: (example)
# Error: file_handles differ. Removing object.
if [ -e "${newname7}" ]; then
    echo ""
    echo "7: Wrong Directory Test FAILED (file was not deleted)"
fi

echo "Alright, test is all set!"
