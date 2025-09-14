# mygit - Attempting to make git in C++

To use/test the git functions:

1. Create a temporary folder to avoid damaging the local copy of the repo's `.git` folder:
```bash
mkdir -p /tmp/testing && cd /tmp/testing
```
2. Create an alias to the `mygit.sh` script to use it:
```bash
alias mygit=~/path/to/repo/mygit.sh
```
3. Test out the commands using `mygit`:
```bash
mygit hash-object -w text.txt
```
