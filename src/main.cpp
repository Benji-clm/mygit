#include <cstdio>
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <string>
#include <zlib.h>
#include <vector>
#include <openssl/sha.h>
#include <unistd.h>
#include <getopt.h>
#include <vector>
#include <cerrno>
#include <algorithm>

#define CHUNK 16384

std::vector<unsigned char> make_prefix(size_t bytes) {
    std::string s = "blob " + std::to_string(bytes) + '\0';
    return std::vector<unsigned char>(s.begin(), s.end());
}

int compress(FILE *source, FILE *dest, size_t bytes, std::vector<unsigned char> prefix, int level = 6){

  int ret, flush;
  z_stream strm;
  unsigned have;
  unsigned char in[CHUNK];
  unsigned char out[CHUNK];

  // Allocate deflate state
  strm.zalloc = Z_NULL;
  strm.zfree = Z_NULL;
  strm.opaque = Z_NULL;
  ret = deflateInit(&strm, level);
  if(ret != Z_OK){
    std::cerr << "Error starting zlib compression \n";
    return ret;
  }


  strm.next_in  = prefix.data();
  strm.avail_in = (uInt)prefix.size();
  flush = Z_NO_FLUSH;

  while (strm.avail_in > 0) {
      strm.next_out  = out;
      strm.avail_out = CHUNK;

      ret = deflate(&strm, flush);
      assert(ret != Z_STREAM_ERROR);

      have = CHUNK - strm.avail_out;
      if (have > 0) {
          if (fwrite(out, 1, have, dest) != have || ferror(dest)) {
              deflateEnd(&strm);
              return Z_ERRNO;
          }
      }
  }

  fseek(source, 0, SEEK_SET);

  // Decompress until eof
  do {
    strm.avail_in = fread(in, 1, CHUNK, source);
    if(ferror(source)){
      (void)deflateEnd(&strm);
      return Z_ERRNO;
    }

    flush = feof(source) ? Z_FINISH : Z_NO_FLUSH;
    strm.next_in = in;

    // run deflate() on input until output buffer is not full
    // finish compression if all of source has been read in
    do {
      strm.avail_out = CHUNK;
      strm.next_out = out;
      ret = deflate(&strm, flush);
      assert(ret != Z_STREAM_ERROR);
      have = CHUNK - strm.avail_out;
      if (fwrite(out, 1, have, dest) != have || ferror(dest)) {
          (void)deflateEnd(&strm);
          return Z_ERRNO;
      }

    } while(strm.avail_out == 0);
    assert(strm.avail_in == 0); // should have consumed al of the input

  } while (flush != Z_FINISH);

  (void)deflateEnd(&strm);
  return Z_OK;
}

std::string get_mode(int mode_val){
  switch(mode_val){
    case(100644): return "blob";
    case(100755): return "blob";
    case(120000): return "blob";
    case(160000): return "commit";
    case(040000): return "tree";
    default:
      std::cerr << "Error, unrecognised mode value \n";
      return "";
  }
}


void parse_git_tree(std::vector<unsigned char> data, bool name_only){
  if(std::string_view(reinterpret_cast<const char*>(&data[0]), 4) != "tree"){
    std::cerr << "ls-tree error: trying to read a non-tree object \n";
    return;
  }

  const uint8_t* begin = data.data();
  const uint8_t* end = data.data() + data.size();
  const uint8_t* p = std::find(begin, end, '\0');
  ++p; // make p point after the header
  const uint8_t* n_jump;

  std::string mode{""};
  std::string mode_val{""};
  std::string name{""};

  while(p < end){
    n_jump = std::find(p , end, ' ');
    mode_val.assign(reinterpret_cast<const char*>(p), n_jump-p);
    mode = get_mode(std::stoi(mode_val));
    p = n_jump;
    ++p;

    n_jump = std::find(p , end, '\0');
    name.assign(reinterpret_cast<const char*>(p), n_jump-p);
    p = n_jump;
    ++p;


    if(p + 20 > end){
      std::cerr << "Invalid tree: truncated SHA-1 \n";
      return;
    }

    std::ostringstream sha_hex;
    sha_hex << std::hex << std::setfill('0') << std::nouppercase;

    for(int i = 0; i < 20; ++i){
      sha_hex << std::setw(2) << static_cast<unsigned>(p[i]);
    }

    if(name_only) std::cout << name << std::endl;
    else std::cout << mode_val << " " << mode << " " << sha_hex.str() << "    " << name << std::endl;
    p += 20;
  }
}

std::vector<unsigned char> get_contents(std::string path){
  std::vector<unsigned char> contents;
  std::ifstream f(path, std::ios::binary);
  if (!f) { std::cerr << " open failed\n"; return contents;}
  std::vector<unsigned char> comp((std::istreambuf_iterator<char>(f)),
                                  std::istreambuf_iterator<char>());

  z_stream strm{};
  strm.next_in = comp.data();
  strm.avail_in = comp.size();

  unsigned char buf[CHUNK];
  size_t have;
  int ret;
  ret = inflateInit(&strm);
  if(ret != Z_OK) std::cerr << "Inflate error: " << ret << std::endl;

  do{
    strm.avail_out = CHUNK;
    strm.next_out = buf;

    ret = inflate(&strm, Z_NO_FLUSH);
    if (ret != Z_OK && ret != Z_STREAM_END){
      std::cerr << "inflate error: " << ret << std::endl;
      inflateEnd(&strm);
      return contents;
    }

    have = CHUNK - strm.avail_out;
    contents.insert(contents.end(), buf, buf + have);
  } while(ret != Z_STREAM_END);

  inflateEnd(&strm);
  return contents;
}


int read_zlib(std::string path, bool pretty_print){
  std::ifstream f(path, std::ios::binary);
  if (!f) { std::cerr << "open failed\n"; return 1; }
  std::vector<unsigned char> comp((std::istreambuf_iterator<char>(f)),
                                   std::istreambuf_iterator<char>());

  z_stream strm{};
  strm.next_in = comp.data();
  strm.avail_in = comp.size();

  unsigned char buf[CHUNK];
  size_t have;
  int ret;
  ret = inflateInit(&strm);
  if(ret != Z_OK) return ret;

  std::string out;

  /* run inflate() on input until output buffer not full */
  do{
    strm.avail_out = CHUNK;
    strm.next_out = buf;

    ret = inflate(&strm, Z_NO_FLUSH);
    if (ret != Z_OK && ret != Z_STREAM_END) {
        std::cerr << "inflate error: " << ret << "\n";
        inflateEnd(&strm);
        return 1;
    }

    have = CHUNK - strm.avail_out;
    out.insert(out.end(), buf, buf + have);

  } while(ret != Z_STREAM_END);

  inflateEnd(&strm);

  std::string contents(out.begin(), out.end());

  if(pretty_print){
    size_t nullpos = contents.find('\0');

    if (nullpos != std::string::npos) {
        const auto blob_data = contents.substr(nullpos + 1);
        std::cout << blob_data;
    }
  }  else {
    std::cout << contents;
  }

  return Z_OK;
}

int main(int argc, char *argv[])
{
    // Flush after every std::cout / std::cerr
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    // You can use print statements as follows for debugging, they'll be visible when running tests.
    std::cerr << "Logs from your program will appear here!\n";

    // Uncomment this block to pass the first stage

    if (argc < 2) {
        std::cerr << "No command provided.\n";
        return EXIT_FAILURE;
    }

    std::string command = argv[1];

  if (command == "init") {
        try {
            std::filesystem::create_directory(".git");
            std::filesystem::create_directory(".git/objects");
            std::filesystem::create_directory(".git/refs");

            std::ofstream headFile(".git/HEAD");
            if (headFile.is_open()) {
                headFile << "ref: refs/heads/main\n";
                headFile.close();
            } else {
                std::cerr << "Failed to create .git/HEAD file.\n";
                return EXIT_FAILURE;
            }

            std::cout << "Initialized git directory\n";
        } catch (const std::filesystem::filesystem_error& e) {
            std::cerr << e.what() << '\n';
            return EXIT_FAILURE;
        }
  } else if(command == "cat-file"){
    if(argc < 3) {
      std::cerr << "Invalid number of arguments - git cat-file requires at least one valid blob-sha \n";
      return EXIT_FAILURE;
    }
    // Need to first read the parameters, starting with a -
    bool pretty_print = false;

    int c;
    optind = 2;

    while((c = getopt(argc, argv, "p")) != -1){
      switch(c){
        case 'p':
          pretty_print = true;
          break;
        default:
          fprintf (stderr,
                   "Unknown option character `\\x%x'.\n",
                   optopt);
          return 1;
      }
    }

    if (argc - optind != 1) {
        fprintf(stderr, "Usage: %s [-p] <blob_sha>\n", argv[0]);
        return 1;
    }

    std::string blob_sha = argv[optind];
    std::string path = ".git/objects/" + blob_sha.substr(0, 2) + "/" + blob_sha.substr(2);

    if(read_zlib(path, pretty_print) != Z_OK){
      std::cerr << "Error reading objet at: " << path << std::endl;
      return EXIT_FAILURE;
    }

  } else if(command == "hash-object"){
    if (argc < 3){ std::cerr << "Error: Missing one or more arguments - git hash-object -w <file.txt> \n";
      return EXIT_FAILURE;
    }

    bool write_obj{false};

    int c;
    optind = 2;

    while((c = getopt(argc, argv, "w")) != -1){
      switch (c) {
        case 'w':
          write_obj = true;
          break;
        default:
          fprintf(stderr, "Unknown option character `\\x%x'.\n", optopt);
          return 1;
      }
    }

    if (argc - optind != 1){
      fprintf(stderr, "Usage: %s [-w] <blob_sha>\n", argv[0]);
    }

    std::string in_file = argv[optind];

    FILE* in_f = std::fopen(in_file.c_str(), "r");
    if(in_f == nullptr){
      std::cerr << "Failed to open input file: " << in_file << std::endl;
      return EXIT_FAILURE;
    }

    unsigned char buffer[CHUNK];
    SHA_CTX ctx;
    SHA1_Init(&ctx);

    size_t total_bytes{0};
    size_t read_size = fread(buffer, 1, CHUNK, in_f);
    total_bytes += read_size; // note that we have to get rid of 1 byte at the end due to the eol not being needed in the zlib format

    while(read_size != 0) {
      read_size = fread(buffer, 1, CHUNK, in_f);
      total_bytes += read_size;
    }

    auto prefix = make_prefix(total_bytes);
    SHA1_Update(&ctx, prefix.data(), prefix.size());
    SHA1_Update(&ctx, buffer, total_bytes);

    unsigned char hash[SHA_DIGEST_LENGTH];
    if(SHA1_Final(hash,&ctx) != 1){
      std::cout << "Error producing the git-hash (SHA1_Final error) \n";
      return EXIT_FAILURE;
    };

    static const char hex_digits[] = "0123456789abcdef";
    char hex[SHA_DIGEST_LENGTH * 2 + 1];

    for(int i = 0; i < SHA_DIGEST_LENGTH; ++i){
      unsigned char b = hash[i];
      hex[i*2] = hex_digits[b >> 4];
      hex[i*2 + 1] = hex_digits[b & 0x0F];
    }

    hex[40] = '\0';

    // char dirname[3];
    // dirname[0] = hex[0];
    // dirname[1] = hex[1];
    // dirname[2] = '\n';
    if(write_obj){
      std::string dirname = ".git/objects/" + std::string(hex).substr(0, 2);

      if(!std::filesystem::create_directory(dirname)){
        std::cerr << "Error creating output directory: " << dirname << std::endl;
        return EXIT_FAILURE;
      }

      std::string outfile = dirname + "/" + std::string(hex).substr(2);

      FILE* dest = fopen(outfile.c_str(), "wb");
      if(!dest){
        std::cerr << "Failed to create output file: " << outfile << std::endl;
        return EXIT_FAILURE;
      }

      if(compress(in_f, dest, total_bytes, prefix) != Z_OK){
        std::cerr << "Failed to create compressed zlib file \n";
        return EXIT_FAILURE;
      }
    }

    for (int i = 0; i < SHA_DIGEST_LENGTH; i++)
        printf("%02x", hash[i]);
    printf("\n");

  } else if(command == "ls-tree"){
    if(argc < 3) {
      std::cerr << "Invalid number of arguments - git ls-tree requires at least one valid tree-ish sha\n";
      return EXIT_FAILURE;
    }
    // Need to first read the parameters, starting with a -
    int name_only;
    optind = 2; // set optind to 2 to skip mygit & ls-tree

    static struct option long_options[] = {
      {"name-only", no_argument, &name_only, 1}
    };

    int option_index = 0;
    getopt_long(argc, argv, "", long_options, &option_index);

    if (argc - optind != 1){
      fprintf(stderr, "Usage: %s [-w] <blob_sha>\n", argv[0]);
    }


    std::string tree_sha = argv[optind];
    std::string path = ".git/objects/" + tree_sha.substr(0, 2) + "/" + tree_sha.substr(2);

    std::vector<unsigned char> contents = get_contents(path);

    parse_git_tree(contents, name_only);
  } else {
        std::cerr << "Unknown command " << command << '\n';
        return EXIT_FAILURE;
  }

    return EXIT_SUCCESS;
}
