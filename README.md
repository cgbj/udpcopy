# udpcopy
udp引量工具，在原开源版本基础上新加了控制qps功能

用法跟原开源版本一致，新增qps功能参数为-q （send num per second）
用法示例：
./udpcopy -c 1.1.1.1 -x 123.194.0.236:1234-123.137.0.39:1234 -q 100
