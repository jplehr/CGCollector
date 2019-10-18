

cgcollectorExe=cgcollector
testerExe=cgsimpletester

command -v $testerExe
if [ $? -eq 1 ]; then
	echo "The CGSimpleTester binary (cgsimpletester) could not be found in path, testing with relative path."
fi
stat ../build/test/cgsimpletester > /dev/null
if [ $? -eq 1 ]; then
	echo "The file seems also non-present in ../build/test. Aborting test. Failure! Please build the tester first."
	exit 1
else
	testerExe=../build/test/cgsimpletester
fi


command -v cgcollectorExe
if [ $? -eq 1 ]; then
	echo "No cgcollector in PATH. Trying relative path ../build/tools"
fi
stat ../build/tools/cgcollector > /dev/null
if [ $? -eq 1 ]; then
	echo "The file seems also non-present in ../build/tools. Aborting test. Failure! Please build the collector first."
	exit 1
else
  cgcollectorExe=../build/tools/cgcollector
fi

tests=(0001 0002 0003 0004 0005 0006 0007 0008 0009 0010 0011 0012 0013 0014 0015 0016 0017 0018 0019 0020 0021)

failures=0

for tc in ${tests[@]}; do
	tfile=$tc.cpp
	gfile=$tc.ipcg
	tgt=$tc.gtipcg


	$cgcollectorExe ./input/$tfile --
	$testerExe ./input/$tgt ./input/$gfile
	if [ $? -eq 1 ]; then
		echo "Failure for file: $gfile. Keeping generated file for inspection"
		let failures= $failures + 1
	else
		rm ./input/$gfile
	fi
done

echo "$failures failures occured when running tests"
exit $failures
