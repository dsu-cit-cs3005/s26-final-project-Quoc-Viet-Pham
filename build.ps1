$ErrorActionPreference = "Stop"

g++ -std=c++20 -Wall -Wextra -pedantic -c RobotBase.cpp -o RobotBase.o
g++ -std=c++20 -Wall -Wextra -pedantic test_robot.cpp RobotBase.o -ldl -o test_robot.exe
g++ -std=c++20 -Wall -Wextra -pedantic main.cpp Arena.cpp RobotBase.o -ldl -o RobotWarz.exe

Write-Host "Built test_robot.exe and RobotWarz.exe"
