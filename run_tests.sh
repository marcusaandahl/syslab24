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

#echo -e "${LG}RUNNING TEST 01${NC}"
#../pxedrive24/pxy/pxydrive.py -f ../pxedrive24/s01*.cmd -S 3 -p ./proxy
#read -p "Press any key to continue to next test... " -n1 -s
#clear
#
#
#echo -e "${LG}RUNNING TEST 02${NC}"
#../pxedrive24/pxy/pxydrive.py -f ../pxedrive24/s02*.cmd -S 3 -p ./proxy
#read -p "Press any key to continue to next test... " -n1 -s
#clear
#
#
#echo -e "${LG}RUNNING TEST 03${NC}"
#../pxedrive24/pxy/pxydrive.py -f ../pxedrive24/s03*.cmd -S 3 -p ./proxy
#read -p "Press any key to continue to next test... " -n1 -s
#clear
#
#
#echo -e "${LG}RUNNING TEST 04${NC}"
#../pxedrive24/pxy/pxydrive.py -f ../pxedrive24/s04*.cmd -S 3 -p ./proxy
#read -p "Press any key to continue to next test... " -n1 -s
#clear


echo -e "${LG}RUNNING TEST 05${NC}"
../pxedrive24/pxy/pxydrive.py -f ../pxedrive24/s05*.cmd -S 3 -p ./proxy
read -p "Press any key to finish... " -n1 -s
clear