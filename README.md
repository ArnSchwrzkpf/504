コンパイルと実行
このコードをコンパイルするには、次のコマンドを使用します：

gcc -o http_server http_server.c -lpthread
実行するには、コンパイル後に生成されたhttp_serverを実行します：

./http_server
シグナルを送るには、例えば、kill -HUP <pid>で設定ファイルを再読み込みし、kill -TERM <pid>でサーバーを終了させることができます。
