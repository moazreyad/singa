wget https://downloads.apache.org//creadur/apache-rat-0.13/apache-rat-0.13-bin.tar.bz2
tar -xvf ./apache-rat-0.13-bin.tar.bz2 
java -jar apache-rat-0.13/apache-rat-0.13.jar .

if [ $? != 0 ]; then
    echo "${red}Rat check failed.${reset}"
    exit 1
fi
