clear
echo "KILLING PROCESSES..."
kill -9 $(lsof -t -i:21199)
clear

echo "PULLING LATEST..."
git pull
clear

echo "MAKE CLEAN..."
make clean
clear

echo "MAKE..."
make
clear

echo "STARTING ON 21199"
./proxy 21199