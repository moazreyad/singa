mvn apache-rat:check

if [ $? != 0 ]; then
    echo "${red}Rat check failed.${reset}"
    exit 1
fi
