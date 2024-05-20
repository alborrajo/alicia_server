# alicia_server

## Installation

On Windows using Visual Studio Build Tools and Chocolatey:

1. Install Boost (On an admin terminal: ```choco install boost-msvc-14.3```)

## Compiling

1. cd build

2. cmake -DBOOST_ROOT="C:\local\boost_1_84_0" ..

3. cmake --build .

## Running

build/Debug/alicia-server.exe