cd java
mvn apache-rat:check -Pcheck-rat -Drat.basedir=..
cat target/rat.txt
