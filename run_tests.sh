LG='\033[1;32m' # Light Green
NC='\033[0m' # No Color

echo -e "${LG}GIT PULL...${NC}"
git pull
clear

echo -e "${LG}MAKE CLEAN...${NC}"
make clean
clear

echo -e "${LG}MAKE...${NC}"
make
clear


echo -e "${LG}RUNNING TEST 05${NC}"
../pxedrive24/pxy/pxydrive.py -f ../pxedrive24/s05*.cmd -S 3 -p ./proxy
read -p "Press any key to finish... " -n1 -s
clear