#!/bin/sh

VER="-$(date +"%Y-%m-%d"-%H%M)-"
CODE="Experi"
BASEVER="Psycho-"
if [ "$TRAVIS_PULL_REQUEST" == "false" ]; then
  echo -e "Starting to update gh-pages\n"

  #copy data we're interested in to other place
  mkdir $HOME/Builds
  cd $HOME/kernel/flashable
  zip -r `echo $BASEVER$CODE$VER`.zip *
  mv `echo $BASEVER$CODE$VER`.zip $HOME/Builds

  #go to home and setup git
  cd $HOME
  git config --global user.email "travis@travis-ci.org"
  git config --global user.name "Travis"

  #using token clone gh-pages branch
  git clone --quiet --branch=gh-pages https://${GH_TOKEN}@github.com/psycho-source/Psycho-Kernel.git  gh-pages > /dev/null

  #go into diractory and copy data we're interested in to that directory
  cp -Rf $HOME/Builds/* $HOME/gh-pages/Experimental/

  #add, commit and push files
  git add -f .
  git commit -m "Travis build $TRAVIS_BUILD_NUMBER pushed to gh-pages"
  git push -fq origin gh-pages > /dev/null

  echo -e "Done\n"
fi