#!/bin/sh 

# run test images through libnsbmp and count results

TEST_PATH=$1
TEST_OUT=${TEST_PATH}/ppm
TEST_LOG=${TEST_PATH}/test.log

mkdir -p ${TEST_OUT}

echo "Bitmap tests" > ${TEST_LOG}

# bitmap test directories

# standard bitmap suite
BMPTESTS="test/bmpsuite/*.bmp"

# netsurf test bitmaps
BMPTESTS="${BMPTESTS} test/bmp/*.bmp"

# afl bitmap suite
BMPTESTS="${BMPTESTS} test/afl-bmp/*.bmp"

# netsurf afl generated bitmap suite
BMPTESTS="${BMPTESTS} test/ns-afl-bmp/*.bmp"

# icon test directories
ICOTESTS="test/icons/*.ico"

# afl ico demo suite
ICOTESTS="${ICOTESTS} test/afl-ico/*.ico"

# netsurf afl generated icon corpus
ICOTESTS="${ICOTESTS} test/ns-afl-ico/*.ico"

bmpdecode()
{
    OUTF=$(basename ${1} .bmp)
    echo "Bitmap:${1}" >> ${TEST_LOG}
    ${TEST_PATH}/test_decode_bmp ${1} ${TEST_OUT}/${OUTF}.ppm 2>> ${TEST_LOG}
    ECODE=$?
    echo "Exit code:${ECODE}" >> ${TEST_LOG}
    return ${ECODE}
}

icodecode()
{
    OUTF=$(basename ${1} .ico)
    CMPF=$(dirname  ${1})/${OUTF}.ppm
    echo "Icon:${1}" >> ${TEST_LOG}
    ${TEST_PATH}/test_decode_ico ${1} 255 255 ${TEST_OUT}/${OUTF}.ppm 2>> ${TEST_LOG}
    if [ -f "${CMPF}" ]; then
	cmp ${CMPF} ${TEST_OUT}/${OUTF}.ppm >> ${TEST_LOG} 2>> ${TEST_LOG}
	if [ "$?" -ne 0 ]; then
	    return 128
	fi
    fi
}

# bitmap tests

BMPTESTTOTC=0
BMPTESTPASSC=0
BMPTESTERRC=0

for BMP in $(ls ${BMPTESTS});do
    BMPTESTTOTC=$((BMPTESTTOTC+1))
    bmpdecode ${BMP}
    ECODE=$?
    if [ \( "${ECODE}" -gt 127 \) -o \( "${ECODE}" -eq 1 \) ];then
	BMPTESTERRC=$((BMPTESTERRC+1))
    else
	BMPTESTPASSC=$((BMPTESTPASSC+1))
    fi
done

echo "Test bitmap decode"
echo "Tests:${BMPTESTTOTC} Pass:${BMPTESTPASSC} Error:${BMPTESTERRC}"


# icon tests

ICOTESTTOTC=0
ICOTESTPASSC=0
ICOTESTERRC=0

# netsurf test icons
for ICO in $(ls ${ICOTESTS});do
    ICOTESTTOTC=$((ICOTESTTOTC+1))
    icodecode ${ICO}
    ECODE=$?
    if [ \( "${ECODE}" -gt 127 \) -o \( "${ECODE}" -eq 1 \) ];then
	ICOTESTERRC=$((ICOTESTERRC+1))
    else
	ICOTESTPASSC=$((ICOTESTPASSC+1))
    fi
done

echo "Test icon decode"
echo "Tests:${ICOTESTTOTC} Pass:${ICOTESTPASSC} Error:${ICOTESTERRC}"



# exit code
if [ "${BMPTESTERRC}" -gt 0 -o "${ICOTESTERRC}" -gt 0 ]; then
    exit 1
fi

exit 0
