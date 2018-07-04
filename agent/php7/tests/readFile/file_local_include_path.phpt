--TEST--
hook file
--DESCRIPTION--
http://php.net/manual/en/function.file.php
local file with include path
--SKIPIF--
<?php
$plugin = <<<EOF
plugin.register('readFile', params => {
    assert(params.path == 'tmpfile')
    assert(params.realpath == '/tmp/openrasp/tmpfile')
    return block
})
EOF;
include(__DIR__.'/../skipif.inc');
file_put_contents('/tmp/openrasp/tmpfile', 'temp');
?>
--INI--
openrasp.root_dir=/tmp/openrasp
include_path=.:/tmp/openrasp
--FILE--
<?php
var_dump(file('tmpfile', FILE_USE_INCLUDE_PATH));
?>
--EXPECTREGEX--
<\/script><script>location.href="http[s]?:\/\/.*?request_id=[0-9a-f]{32}"<\/script>