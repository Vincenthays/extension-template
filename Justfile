[macos]
upload:
    @cat build/release/extension/bigtable2/bigtable2.duckdb_extension | gzip | gsutil cp - gs://di_duckdb_extension/v1.1.3/osx_arm64/bigtable2.duckdb_extension.gz

[linux]
