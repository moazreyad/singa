cd java
mvn apache-rat:check -Pcheck-rat -Drat.basedir=..

if [ $? != 0 ]; then
    echo "${red}Rat check failed.${reset}"
    exit 1
fi
