#!/bin/sh

# run test images through libnsgif and count results

TEST_PATH=$1
TEST_OUT=${TEST_PATH}/ppm
TEST_LOG=${TEST_PATH}/test.log

mkdir -p ${TEST_OUT}

echo "GIF tests" > ${TEST_LOG}

# gif test directories

# netsurf test gif
GIFTESTS="test/data/*.gif"

# netsurf afl generated gif suite
GIFTESTS="${GIFTESTS} test/ns-afl-gif/*.gif"

gifdecode()
{
	OUTF=$(basename ${1} .gif)
	CMPF=$(dirname  ${1})/${OUTF}.ppm
	echo "GIF:${1}" >> ${TEST_LOG}
	${TEST_PATH}/test_nsgif ${1} --ppm ${TEST_OUT}/${OUTF}.ppm 2>> ${TEST_LOG}
	ECODE=$?

	echo "Exit code:${ECODE}" >> ${TEST_LOG}
	if [ "${ECODE}" -gt 0 ];then
		return ${ECODE}
	fi

	if [ -f "${CMPF}" ]; then
		cmp ${CMPF} ${TEST_OUT}/${OUTF}.ppm >> ${TEST_LOG} 2>> ${TEST_LOG}
		if [ "$?" -ne 0 ]; then
			return 128
		fi
	fi

	return 0
}

GIFTESTTOTC=0
GIFTESTPASSC=0
GIFTESTFAILC=0
GIFTESTERRC=0

echo "Testing GIF decode"

for GIF in $(ls ${GIFTESTS});do
	GIFTESTTOTC=$((GIFTESTTOTC+1))
	#echo "${GIF}"
	gifdecode ${GIF}
	ECODE=$?
	if [ "${ECODE}" -gt 127 ];then
		GIFTESTERRC=$((GIFTESTERRC+1))
		echo "Error ${GIF}"
	else
		if [ "${ECODE}" -gt 0 ];then
			GIFTESTFAILC=$((GIFTESTFAILC+1))
		else
			GIFTESTPASSC=$((GIFTESTPASSC+1))
		fi
	fi
done

echo "Tests:${GIFTESTTOTC} Pass:${GIFTESTPASSC} Fail:${GIFTESTFAILC} Error:${GIFTESTERRC}"

# exit code
if [ "${GIFTESTERRC}" -gt 0 ]; then
	exit 1
fi

exit 0
