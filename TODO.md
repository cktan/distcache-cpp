- for GET /file?delete=1: consider just setting mtime of the file to 0 in LRU
- when streaming file data, investigate if there is an INFO meta that carries
  the file size before DATA frames.
  
